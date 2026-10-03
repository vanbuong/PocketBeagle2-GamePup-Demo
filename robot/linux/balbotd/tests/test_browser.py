# SPDX-License-Identifier: GPL-2.0-only
"""The control page in a real Chromium against the real app (fake M4F, test-pattern camera).

Needs `pip install playwright` and a Chromium: set BALBOT_CHROMIUM, or have /opt/pw-browsers/chromium-*/chrome-linux/chrome,
or run `playwright install chromium`. Set BALBOT_SHOTS=<dir> to keep screenshots.
"""
import glob
import os
import time

import pytest

playwright = pytest.importorskip("playwright.sync_api")
from live import Live, make_client  # noqa: E402


def chromium_path():
    p = os.environ.get("BALBOT_CHROMIUM")
    if p:
        return p
    found = sorted(glob.glob("/opt/pw-browsers/chromium-*/chrome-linux/chrome"))
    if found:
        return found[-1]
    try:
        with playwright.sync_playwright() as pw:
            return pw.chromium.executable_path
    except Exception:
        return None


CHROME = chromium_path()
pytestmark = pytest.mark.skipif(not CHROME or not os.path.exists(CHROME), reason="no Chromium available")

LANDSCAPE = {"width": 844, "height": 390}
PORTRAIT = {"width": 390, "height": 844}


@pytest.fixture(scope="module")
def browser():
    with playwright.sync_playwright() as pw:
        b = pw.chromium.launch(executable_path=CHROME, args=["--no-sandbox"])
        yield b
        b.close()


@pytest.fixture
def live():
    app, toks = make_client()
    lv = Live(app, toks)
    lv.fake = app.state.fake_m4
    lv.sup = app.state.supervisor
    yield lv
    lv.stop()


class Page:
    def __init__(self, browser, live, viewport=LANDSCAPE, touch=False, scheme="light"):
        self.live = live
        self.ctx = browser.new_context(viewport=viewport, has_touch=touch, color_scheme=scheme)
        self.page = self.ctx.new_page()
        self.errors = []
        self.page.on("console", lambda m: self.errors.append(m.text) if m.type == "error" else None)
        self.page.on("pageerror", lambda e: self.errors.append(str(e)))

    def open(self, token=None, fragment=False):
        url = self.live.base + "/ui/" + (f"#token={token}" if fragment and token else "")
        self.page.goto(url)
        if token and not fragment:
            self.login(token)
        return self

    def login(self, token):
        self.page.fill("#token", token)
        self.page.click("#login-go")

    def wait_fn(self, js, arg=None, timeout=8000):
        """Poll a JS expression/function with evaluate() (CDP): Playwright's wait_for_function uses eval(), which the page's
        strict CSP correctly refuses, and we want the CSP to stay on in tests."""
        fn = js if "=>" in js else f"() => ({js})"
        end = time.monotonic() + timeout / 1000
        while True:
            val = self.page.evaluate(fn, arg) if arg is not None else self.page.evaluate(fn)
            if val:
                return val
            if time.monotonic() > end:
                raise AssertionError(f"timeout waiting for {js[:120]}")
            time.sleep(0.04)

    def text(self, sel):
        return self.page.inner_text(sel).strip()

    def wait_text(self, sel, expected, timeout=8000):
        self.wait_fn("([s, e]) => document.querySelector(s).textContent.trim() === e", arg=[sel, expected], timeout=timeout)

    def wait_state(self, state, timeout=8000):
        self.wait_text("#v-state", state, timeout)

    def wait_enabled(self, sel, timeout=8000):
        self.wait_fn("s => !document.querySelector(s).disabled", arg=sel, timeout=timeout)

    def take_control(self):
        self.wait_enabled("#btn-control")
        self.page.click("#btn-control")
        self.wait_text("#btn-control", "Release control")

    def arm(self):
        self.wait_state("standby")
        self.take_control()
        self.wait_enabled("#btn-arm")
        self.page.click("#btn-arm")
        self.wait_state("balancing", 10000)

    def center(self, sel):
        b = self.page.locator(sel).bounding_box()
        return b["x"] + b["width"] / 2, b["y"] + b["height"] / 2

    def drag(self, sel, dx, dy):
        cx, cy = self.center(sel)
        self.page.mouse.move(cx, cy)
        self.page.mouse.down()
        self.page.mouse.move(cx + dx, cy + dy, steps=6)

    def shot(self, name):
        d = os.environ.get("BALBOT_SHOTS")
        if d:
            os.makedirs(d, exist_ok=True)
            self.page.screenshot(path=os.path.join(d, name + ".png"))

    def close(self):
        self.ctx.close()


@pytest.fixture
def mk(browser, live):
    pages = []

    def make(viewport=LANDSCAPE, touch=False, scheme="light"):
        p = Page(browser, live, viewport, touch, scheme)
        pages.append(p)
        return p

    yield make
    for p in pages:
        p.close()


def wait_for(fn, timeout=6.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if fn():
            return True
        time.sleep(0.02)
    return False


# ---- connect -----------------------------------------------------------------------------------------------
def test_login_rejects_a_bad_token_then_accepts_the_right_one(mk, live):
    p = mk().open()
    assert p.page.is_visible("#login")
    p.login("definitely-wrong")
    p.page.wait_for_selector("#login-error:not([hidden])")
    assert "not accepted" in p.text("#login-error") and p.page.is_visible("#login")
    p.login(live.tokens["admin"])
    p.wait_text("#v-conn", "online")
    assert not p.page.is_visible("#login")
    p.wait_state("standby")
    p.wait_text("#v-batt", "11.1 V")
    assert p.text("#v-driver") == "none"


def test_token_in_the_url_fragment_connects_and_is_removed_from_the_address(mk, live):
    p = mk().open(live.tokens["admin"], fragment=True)
    p.wait_text("#v-conn", "online")
    assert p.page.evaluate("location.hash") == ""
    assert not p.page.is_visible("#login")
    assert not p.errors, p.errors


def test_the_token_survives_a_reload_in_the_same_tab_only(mk, live):
    p = mk().open(live.tokens["admin"])
    p.wait_text("#v-conn", "online")
    p.page.reload()
    p.wait_text("#v-conn", "online")  # sessionStorage
    assert p.page.evaluate("localStorage.length") == 0, "the token must not be persisted beyond the tab"
    p2 = mk().open()
    assert p2.page.is_visible("#login")


# ---- driving -----------------------------------------------------------------------------------------------
def test_arm_and_drive_with_the_mouse_stick_then_release(mk, live):
    p = mk().open(live.tokens["admin"])
    p.arm()
    assert p.text("#v-driver") == "admin"
    p.drag("#stick-speed", 0, -70)  # up = forward
    p.wait_fn("parseFloat(document.querySelector('#t-v').textContent) > 0.15", timeout=6000)
    assert live.fake._v_target > 0.5 and live.fake._w_target == 0
    p.shot("driving-landscape")
    p.page.mouse.up()
    assert wait_for(lambda: live.fake._v_target == 0.0, 1.0), "releasing the stick must stop the command at once"
    p.wait_fn("parseFloat(document.querySelector('#t-v').textContent) < 0.05", timeout=6000)
    assert not p.errors, p.errors


def test_stick_directions_match_the_robot_conventions(mk, live):
    p = mk().open(live.tokens["admin"])
    p.arm()
    p.drag("#stick-speed", 0, 70)  # down = reverse
    assert wait_for(lambda: live.fake._v_target < -0.3)
    p.page.mouse.up()
    assert wait_for(lambda: live.fake._v_target == 0.0, 1.0)
    p.drag("#stick-turn", -70, 0)  # left = positive yaw (turn left)
    assert wait_for(lambda: live.fake._w_target > 1.0)
    p.page.mouse.up()
    p.drag("#stick-turn", 70, 0)
    assert wait_for(lambda: live.fake._w_target < -1.0)
    p.page.mouse.up()


def test_multi_touch_drives_speed_and_turn_at_the_same_time(mk, live):
    p = mk(touch=True).open(live.tokens["admin"])
    p.arm()
    cdp = p.ctx.new_cdp_session(p.page)
    (sx, sy), (tx, ty) = p.center("#stick-speed"), p.center("#stick-turn")

    def touch(kind, pts):
        cdp.send("Input.dispatchTouchEvent", {"type": kind, "touchPoints": pts})

    touch("touchStart", [{"x": sx, "y": sy, "id": 1}, {"x": tx, "y": ty, "id": 2}])
    touch("touchMove", [{"x": sx, "y": sy - 70, "id": 1}, {"x": tx - 70, "y": ty, "id": 2}])
    assert wait_for(lambda: live.fake._v_target > 0.5 and live.fake._w_target > 1.0), (live.fake._v_target, live.fake._w_target)
    touch("touchEnd", [])
    assert wait_for(lambda: live.fake._v_target == 0.0 and live.fake._w_target == 0.0, 1.0)


def test_keyboard_drive_and_boost(mk, live):
    p = mk().open(live.tokens["admin"])
    p.arm()
    p.page.keyboard.down("KeyW")
    assert wait_for(lambda: abs(live.fake._v_target - 0.6) < 0.01)
    p.page.keyboard.down("ShiftLeft")
    assert wait_for(lambda: abs(live.fake._v_target - 0.96) < 0.01), "boost raises the limit to the firmware maximum"
    p.page.keyboard.up("ShiftLeft")
    p.page.keyboard.down("KeyA")
    assert wait_for(lambda: live.fake._w_target > 1.0)
    p.page.keyboard.up("KeyW")
    p.page.keyboard.up("KeyA")
    assert wait_for(lambda: live.fake._v_target == 0.0 and live.fake._w_target == 0.0, 1.0)


def test_losing_window_focus_stops_the_robot(mk, live):
    p = mk().open(live.tokens["admin"])
    p.arm()
    p.page.keyboard.down("KeyW")
    assert wait_for(lambda: live.fake._v_target > 0.3)
    p.page.evaluate("window.dispatchEvent(new Event('blur'))")
    assert wait_for(lambda: live.fake._v_target == 0.0, 1.0)


def test_hiding_the_tab_releases_control(mk, live):
    p = mk().open(live.tokens["admin"])
    p.arm()
    p.page.evaluate("Object.defineProperty(document, 'hidden', {value: true, configurable: true});"
                    "document.dispatchEvent(new Event('visibilitychange'))")
    p.wait_text("#btn-control", "Take control")
    assert wait_for(lambda: live.sup.arbiter.lease is None)
    assert live.fake.state == 3  # still balancing, just standing still


# ---- safety ------------------------------------------------------------------------------------------------
def test_space_bar_is_an_emergency_stop_and_reset_recovers(mk, live):
    p = mk().open(live.tokens["admin"])
    p.arm()
    p.page.keyboard.down("KeyW")
    assert wait_for(lambda: live.fake._v_target > 0.3)
    p.page.keyboard.press("Space")
    p.wait_state("fault")
    assert live.fake.faults & 4
    p.page.wait_for_selector("#banner:not([hidden])")
    assert "emergency stop" in p.text("#banner") and "Reset fault" in p.text("#banner")
    p.shot("estop-landscape")
    p.page.keyboard.up("KeyW")
    p.wait_enabled("#btn-reset")
    p.page.wait_for_timeout(150)  # E-STOP is repeated for ~45 ms; a reset racing the repeats would lose, safely
    assert p.page.is_disabled("#btn-arm")
    p.page.click("#btn-reset")
    p.wait_state("standby", 10000)
    assert p.page.is_hidden("#banner")


def test_estop_button_works_for_a_viewer_who_cannot_drive(mk, live):
    p = mk().open(live.tokens["viewer"])
    p.wait_state("standby")
    assert p.page.is_disabled("#btn-control") and p.page.is_disabled("#btn-arm")
    p.page.click("#btn-estop")
    p.wait_state("fault")
    assert p.page.is_disabled("#btn-reset"), "a viewer must not be able to reset a fault"


def test_a_second_browser_cannot_steal_control(mk, live):
    a, b = mk().open(live.tokens["admin"]), mk().open(live.tokens["driver"])
    a.take_control()
    b.wait_enabled("#btn-control")
    b.page.click("#btn-control")
    b.page.wait_for_selector("#toast:not([hidden])")
    assert "Someone else is driving" in b.text("#toast")
    assert b.text("#btn-control") == "Take control"
    b.wait_text("#v-driver", "admin")
    a.page.click("#btn-control")  # release
    a.wait_text("#btn-control", "Take control")
    b.wait_text("#v-driver", "none")
    b.page.click("#btn-control")
    b.wait_text("#btn-control", "Release control")


def test_connection_drop_reconnects_without_retaking_control(mk, live):
    p = mk().open(live.tokens["admin"])
    p.take_control()
    p.page.evaluate("window.__balbot.link.ws.close()")
    p.wait_fn("document.querySelector('#app').dataset.conn !== 'online'")
    p.wait_text("#v-conn", "online", timeout=10000)
    assert p.text("#btn-control") == "Take control", "control must be requested again explicitly"
    assert live.sup.arbiter.lease is None or live.sup.arbiter.lease.holder not in live.sup.clients


def test_a_tab_that_stops_updating_input_cannot_leave_the_robot_driving(mk, live):
    p = mk().open(live.tokens["admin"])
    p.arm()
    p.page.keyboard.down("KeyW")
    assert wait_for(lambda: live.fake._v_target > 0.3)
    # freeze the page's input timer (like a throttled background tab) while the key stays "down"
    p.page.evaluate("window.__balbot.controller.update = () => ({})")
    assert wait_for(lambda: live.fake._v_target == 0.0, 2.0), "stale input must time out within about half a second"


# ---- video and layout -------------------------------------------------------------------------------------------
def test_video_shows_frames_and_follows_the_fps_setting(mk, live):
    p = mk().open(live.tokens["admin"])
    p.wait_text("#v-conn", "online")
    p.wait_fn("document.querySelector('#video').naturalWidth > 0", timeout=8000)
    assert p.page.is_hidden("#video-msg") and "fps=30" in p.page.get_attribute("#video", "src")
    p.page.click("#btn-settings")
    p.page.select_option("#video-fps", "5")
    p.wait_fn("document.querySelector('#video').src.includes('fps=5')")
    p.wait_fn("document.querySelector('#video').naturalWidth > 0", timeout=8000)
    p.page.keyboard.press("Escape")


@pytest.mark.parametrize("name,viewport", [("landscape", LANDSCAPE), ("portrait", PORTRAIT), ("small", {"width": 320, "height": 568})])
def test_layout_fits_the_screen_and_keeps_targets_big_enough(mk, live, name, viewport):
    p = mk(viewport).open(live.tokens["admin"])
    p.wait_state("standby")
    p.take_control()
    p.wait_fn("document.querySelector('#video').naturalWidth > 0", timeout=8000)
    w, h = viewport["width"], viewport["height"]
    sw, sh = p.page.evaluate("[document.documentElement.scrollWidth, document.documentElement.scrollHeight]")
    assert sw <= w and sh <= h, f"page scrolls: {sw}x{sh} in {w}x{h}"
    for sel in ("#btn-estop", "#btn-control", "#btn-arm", "#stick-speed", "#stick-turn", "#video"):
        b = p.page.locator(sel).bounding_box()
        assert b and b["x"] >= -1 and b["y"] >= -1 and b["x"] + b["width"] <= w + 1 and b["y"] + b["height"] <= h + 1, (sel, b)
    for sel in ("#btn-estop", "#btn-control", "#btn-arm", "#btn-disarm"):
        b = p.page.locator(sel).bounding_box()
        assert b["height"] >= 38 and b["width"] >= 44, (sel, b)
    e, s1, s2 = (p.page.locator(x).bounding_box() for x in ("#btn-estop", "#stick-speed", "#stick-turn"))

    def overlap(a, b):
        return a["x"] < b["x"] + b["width"] and b["x"] < a["x"] + a["width"] and a["y"] < b["y"] + b["height"] and b["y"] < a["y"] + a["height"]

    assert not overlap(e, s1) and not overlap(e, s2) and not overlap(s1, s2)
    p.shot("layout-" + name)
    assert not p.errors, p.errors


def test_no_csp_violations_or_script_errors_in_a_full_session(mk, live):
    p = mk().open(live.tokens["admin"])
    p.arm()
    p.drag("#stick-speed", 0, -60)
    time.sleep(0.4)
    p.page.mouse.up()
    p.page.click("#btn-settings")
    p.page.keyboard.press("Escape")
    p.page.click("#btn-estop")
    p.wait_state("fault")
    assert not p.errors, p.errors


CONTRAST_JS = """() => {
  const lum = (c) => { const m = c.match(/[\\d.]+/g).map(Number); const f = (v) => { v /= 255; return v <= 0.03928 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4; };
    return { l: 0.2126 * f(m[0]) + 0.7152 * f(m[1]) + 0.0722 * f(m[2]), a: m.length > 3 ? m[3] : 1 }; };
  const ratio = (fg, bg) => { const a = lum(fg).l, b = lum(bg).l; return (Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05); };
  const out = {};
  const pairs = { body: ["body", "body"], chipLabel: ["#chip-conn .label", "#chip-conn"], chipValue: ["#v-state", "#chip-state"],
    estop: ["#btn-estop", "#btn-estop"], primary: ["#btn-arm", "#btn-arm"], control: ["#btn-control", "#btn-control"] };
  for (const [k, [f, b]] of Object.entries(pairs)) {
    const fe = document.querySelector(f), be = document.querySelector(b);
    out[k] = ratio(getComputedStyle(fe).color, getComputedStyle(be).backgroundColor);
  }
  return out;
}"""


@pytest.mark.parametrize("scheme", ["light", "dark"])
def test_text_contrast_meets_wcag_aa_in_both_colour_schemes(mk, live, scheme):
    p = mk(scheme=scheme).open(live.tokens["admin"])
    p.wait_state("standby")
    r = p.page.evaluate(CONTRAST_JS)
    for k, v in r.items():
        assert v >= 4.5, f"{scheme}: {k} contrast {v:.2f}:1 is below 4.5:1"
    p.shot("layout-landscape-" + scheme)
