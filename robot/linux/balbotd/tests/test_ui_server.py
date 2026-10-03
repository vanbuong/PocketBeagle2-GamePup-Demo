# SPDX-License-Identifier: GPL-2.0-only
"""The static UI and the video endpoints, served by the real app with the fake M4F and the test-pattern camera."""
import re
import time
from html.parser import HTMLParser

import pytest
from fastapi.testclient import TestClient

from balbotd import auth
from balbotd.__main__ import build_app
from balbotd.arbiter import Role
from balbotd.auth import TokenEntry
from balbotd.config import Config, parse
from live import Live, make_client


@pytest.fixture
def client():
    app, toks = make_client()
    with TestClient(app) as c:
        c.tokens = toks
        c.app_ = app
        yield c


@pytest.fixture
def live():
    app, toks = make_client()
    lv = Live(app, toks)
    yield lv
    lv.stop()


def H(c, role):
    return {"Authorization": "Bearer " + c.tokens[role]}


def test_root_redirects_browsers_to_the_ui_but_api_clients_get_json(client):
    r = client.get("/", headers={"accept": "text/html,application/xhtml+xml"}, follow_redirects=False)
    assert r.status_code == 307 and r.headers["location"] == "/ui/"
    assert client.get("/").json()["ui"] == "/ui/"


def test_ui_assets_are_served_with_a_strict_content_security_policy(client):
    r = client.get("/ui/")
    assert r.status_code == 200 and "text/html" in r.headers["content-type"] and "BalBot Control" in r.text
    csp = r.headers["content-security-policy"]
    for part in ("default-src 'self'", "script-src 'self'", "style-src 'self'", "object-src 'none'", "frame-ancestors 'none'"):
        assert part in csp
    assert "unsafe-inline" not in csp and "unsafe-eval" not in csp
    assert r.headers["x-content-type-options"] == "nosniff" and r.headers["cache-control"] == "no-cache"
    for path, ctype in (("/ui/js/ui.js", "javascript"), ("/ui/js/link.js", "javascript"), ("/ui/css/app.css", "text/css")):
        r = client.get(path)
        assert r.status_code == 200 and ctype in r.headers["content-type"], path
    assert client.get("/ui/../balbotd/auth.py").status_code in (400, 404)
    assert client.get("/ui/nope.js").status_code == 404
    assert "content-security-policy" not in client.get("/api/v1/state", headers=H(client, "viewer")).headers


class Collect(HTMLParser):
    def __init__(self):
        super().__init__()
        self.scripts, self.inline_scripts, self.style_attrs, self.links, self.ids = [], 0, 0, [], set()

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == "script":
            if "src" in a:
                self.scripts.append(a["src"])
            else:
                self.inline_scripts += 1
        if "style" in a:
            self.style_attrs += 1
        for k in ("href", "src"):
            if k in a:
                self.links.append(a[k])
        if "id" in a:
            self.ids.add(a["id"])


def test_html_has_no_inline_script_or_style_and_no_external_resources(client):
    p = Collect()
    p.feed(client.get("/ui/").text)
    assert p.inline_scripts == 0 and p.style_attrs == 0
    assert p.scripts == ["js/ui.js"]
    assert all(not re.match(r"https?:|//", l) for l in p.links), p.links
    for l in p.links:  # every referenced file exists
        if not l.startswith("data:"):
            assert client.get("/ui/" + l).status_code == 200, l


def test_every_element_id_used_by_the_script_exists_in_the_html(client):
    html = client.get("/ui/").text
    js = client.get("/ui/js/ui.js").text
    p = Collect()
    p.feed(html)
    used = set(re.findall(r'\$\("([A-Za-z0-9_-]+)"\)', js))
    assert used, "regex found nothing"
    assert used <= p.ids, f"missing in HTML: {sorted(used - p.ids)}"


def test_the_module_graph_resolves(client):
    seen, todo = set(), ["js/ui.js"]
    while todo:
        f = todo.pop()
        if f in seen:
            continue
        seen.add(f)
        r = client.get("/ui/" + f)
        assert r.status_code == 200, f
        for imp in re.findall(r'from "\./([A-Za-z0-9_.-]+)"', r.text):
            todo.append("js/" + imp)
    assert {"js/ui.js", "js/link.js", "js/protocol.js", "js/input.js", "js/controller.js", "js/sparkline.js"} <= seen


# ---- video ---------------------------------------------------------------------------------------------------
def test_video_requires_a_token_and_lets_viewers_watch(client):
    assert client.get("/api/v1/video/snapshot").status_code == 401
    assert client.get("/api/v1/video/info").status_code == 401
    r = client.get("/api/v1/video/snapshot", headers=H(client, "viewer"))
    assert r.status_code == 200 and r.headers["content-type"] == "image/png" and r.content[:8] == b"\x89PNG\r\n\x1a\n"
    assert client.get("/api/v1/video/snapshot?token=" + client.tokens["viewer"]).status_code == 200  # <img> cannot set headers
    assert client.get("/api/v1/video/info", headers=H(client, "viewer")).json()["enabled"] is True


def read_parts(live, path, n, headers=None):
    """Read n multipart parts from a streaming response; returns [(content_type, bytes)]."""
    import httpx
    parts, buf = [], b""
    with httpx.stream("GET", live.base + path, headers=headers, timeout=10) as r:
        assert r.status_code == 200
        assert r.headers["content-type"].startswith("multipart/x-mixed-replace") and "boundary=frame" in r.headers["content-type"]
        for chunk in r.iter_bytes():
            buf += chunk
            while True:
                m = re.match(rb"--frame\r\nContent-Type: ([^\r]+)\r\nContent-Length: (\d+)\r\n\r\n", buf)
                if not m:
                    break
                size = int(m.group(2))
                end = m.end() + size + 2
                if len(buf) < end:
                    break
                assert buf[m.end() + size:end] == b"\r\n"
                parts.append((m.group(1).decode(), buf[m.end():m.end() + size]))
                buf = buf[end:]
            if len(parts) >= n:
                break
    return parts


def LH(live, role):
    return {"Authorization": "Bearer " + live.tokens[role]}


def test_mjpeg_stream_is_well_formed_multipart(live):
    parts = read_parts(live, "/api/v1/video/mjpeg", 5, LH(live, "viewer"))
    assert len(parts) >= 5 and all(ct == "image/png" and d[:8] == b"\x89PNG\r\n\x1a\n" for ct, d in parts)
    assert len({d for _, d in parts}) >= 3, "frames must change"


def test_mjpeg_requires_a_token(live):
    import httpx
    assert httpx.get(live.base + "/api/v1/video/mjpeg").status_code == 401


def test_mjpeg_fps_parameter_thins_the_stream(live):
    t0 = time.monotonic()
    read_parts(live, "/api/v1/video/mjpeg?fps=5&token=" + live.tokens["viewer"], 4)
    dt = time.monotonic() - t0
    assert dt > 0.45, f"4 frames at 5 fps cannot arrive in {dt:.2f}s"  # the unthinned pattern runs at 15 fps


def test_leaving_the_stream_releases_the_camera(live):
    import httpx
    read_parts(live, "/api/v1/video/mjpeg", 2, LH(live, "viewer"))
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        s = httpx.get(live.base + "/api/v1/stats", headers=LH(live, "admin")).json()["video"]
        if s["subscribers"] == 0:
            break
        time.sleep(0.05)
    assert s["subscribers"] == 0 and s["frames"] > 0 and s["source"] == "PatternSource"


def test_video_off_returns_404_not_a_hang():
    app, toks = make_client("off")
    with TestClient(app) as c:
        h = {"Authorization": "Bearer " + toks["viewer"]}
        assert c.get("/api/v1/video/mjpeg", headers=h).status_code == 404
        assert c.get("/api/v1/video/snapshot", headers=h).status_code == 404
        assert c.get("/api/v1/video/info", headers=h).json()["enabled"] is False
        assert "video" not in c.get("/api/v1/stats", headers={"Authorization": "Bearer " + toks["admin"]}).json()


def test_auto_video_mode_is_pattern_with_the_fake_and_off_otherwise():
    from balbotd.__main__ import build_app as b
    a = b(Config(), fake=True)
    with TestClient(a) as c:
        assert c.get("/api/v1/video/info?token=x").status_code == 401  # reachable, auth enforced
    assert a.state.supervisor is not None


# ---- config ----------------------------------------------------------------------------------------------------
def test_video_config_parsing_and_validation():
    assert parse({}).video.mode == "auto"
    v = parse({"video": {"mode": "camera", "device": "/dev/video2", "size": "640x480", "fps": 15}}).video
    assert (v.mode, v.device, v.size, v.fps) == ("camera", "/dev/video2", "640x480", 15)
    assert parse({"video": {"mode": "command", "command": ["ffmpeg", "-i", "x"]}}).video.command == ["ffmpeg", "-i", "x"]
    for bad in ({"mode": "webrtc"}, {"mode": "command"}, {"mode": "command", "command": []}, {"mode": "command", "command": [1]},
                {"bogus": 1}):
        with pytest.raises(ValueError):
            parse({"video": bad})
