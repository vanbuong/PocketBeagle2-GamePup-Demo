// SPDX-License-Identifier: GPL-2.0-only
// DOM wiring for the control page. The logic lives in link.js / controller.js / input.js (unit tested in Node).
import { Controller } from "./controller.js";
import { speedFromDrag, yawFromDrag, clamp } from "./input.js";
import { RobotLink } from "./link.js";
import { describeFaults, stateKind } from "./protocol.js";
import { Ring, drawSpark } from "./sparkline.js";

const $ = (id) => document.getElementById(id);
const app = $("app");
const store = {
  get: (k) => { try { return sessionStorage.getItem(k); } catch { return null; } },
  set: (k, v) => { try { sessionStorage.setItem(k, v); } catch { /* private mode */ } },
  del: (k) => { try { sessionStorage.removeItem(k); } catch { /* ignore */ } },
};
const pref = {
  get: (k, d) => { try { return localStorage.getItem(k) ?? d; } catch { return d; } },
  set: (k, v) => { try { localStorage.setItem(k, v); } catch { /* ignore */ } },
};

let link = null;
let controller = null;
let token = null;
let robot = { state: "UNKNOWN", driver: null, faults: [], m4f_alive: false, vbat: null };
let tlm = null;
let tlmDirty = false;
let videoTimer = null;
const pitchRing = new Ring(100); // 5 s at 20 Hz

// ---- small helpers -------------------------------------------------------------------------------------
let toastTimer = null;
function toast(text, ms = 3200) {
  const el = $("toast");
  el.textContent = text;
  el.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.hidden = true; }, ms);
}

const canDrive = () => link && (link.role === "driver" || link.role === "admin");
const online = () => link && link.status === "online";

function refreshButtons() {
  const st = robot.state;
  const leased = !!(link && link.leased);
  const admin = link && link.role === "admin";
  $("btn-control").disabled = !(online() && canDrive());
  $("btn-control").textContent = leased ? "Release control" : "Take control";
  $("btn-arm").disabled = !(online() && (leased || admin) && st === "STANDBY");
  $("btn-disarm").disabled = !(online() && ["BALANCING", "LAYING_DOWN", "FALLEN"].includes(st));
  $("btn-reset").disabled = !(online() && (leased || admin) && st === "FAULT");
  $("btn-estop").disabled = !online();
  app.dataset.leased = leased ? "yes" : "no";
}

function refreshState() {
  const st = robot.state;
  app.dataset.state = st;
  app.dataset.kind = stateKind(st);
  $("v-state").textContent = st === "UNKNOWN" ? "–" : st.toLowerCase().replace("_", " ");
  $("v-driver").textContent = robot.driver ?? "none";
  const batt = $("chip-batt");
  batt.classList.remove("warn", "bad");
  if (robot.vbat == null) {
    $("v-batt").textContent = "–";
  } else {
    $("v-batt").textContent = robot.vbat.toFixed(1) + " V";
    if (robot.vbat < 9.9) batt.classList.add("bad");
    else if (robot.vbat < 10.5) batt.classList.add("warn");
  }
  const banner = $("banner");
  const faults = describeFaults(robot.faults);
  if (st === "FALLEN") {
    banner.textContent = "The robot has fallen. Stand it upright, press Disarm, then Arm.";
  } else if (faults.length) {
    banner.textContent = "Fault: " + faults.join(", ") + (st === "FAULT" ? ". Clear the cause, then press Reset fault." : "");
  } else {
    banner.textContent = "";
  }
  banner.hidden = !banner.textContent;
  refreshButtons();
}

// ---- video ----------------------------------------------------------------------------------------------
function videoMessage(text) {
  const m = $("video-msg");
  m.textContent = text ?? "";
  m.hidden = !text;
}

async function startVideo() {
  clearTimeout(videoTimer);
  const img = $("video");
  try {
    const r = await fetch("/api/v1/video/info", { headers: { Authorization: "Bearer " + token } });
    const info = r.ok ? await r.json() : { enabled: false };
    if (!info.enabled) {
      img.hidden = true;
      img.removeAttribute("src");
      return videoMessage("Video is not enabled on the robot");
    }
  } catch {
    return videoMessage("Video unavailable");
  }
  const fps = Number($("video-fps").value) || 30;
  videoMessage("Starting video…");
  img.onload = () => { videoMessage(""); };
  img.onerror = () => {
    img.hidden = true;
    videoMessage("Video interrupted, retrying…");
    if (online()) videoTimer = setTimeout(startVideo, 2000);
  };
  img.hidden = false;
  img.src = `/api/v1/video/mjpeg?fps=${fps}&token=${encodeURIComponent(token)}&_=${Date.now()}`;
  return undefined;
}

function stopVideo(message) {
  clearTimeout(videoTimer);
  const img = $("video");
  img.onload = img.onerror = null;
  img.hidden = true;
  img.removeAttribute("src");
  videoMessage(message);
}

// ---- link --------------------------------------------------------------------------------------------------
function start(tok) {
  token = tok;
  store.set("balbot.token", tok);
  if (link) link.close();
  link = new RobotLink({
    baseUrl: location.origin,
    token: tok,
    WebSocketCtor: WebSocket,
    fetchImpl: (u, o) => fetch(u, o),
    timers: {
      setInterval: (f, ms) => setInterval(f, ms), clearInterval: (id) => clearInterval(id),
      setTimeout: (f, ms) => setTimeout(f, ms), clearTimeout: (id) => clearTimeout(id),
    },
    now: () => performance.now(),
    subRate: 20,
  });
  controller = new Controller(link);
  robot = { state: "UNKNOWN", driver: null, faults: [], m4f_alive: false, vbat: null };

  link.on("status", ({ status, httpStatus, retryInMs }) => {
    app.dataset.conn = status;
    $("v-conn").textContent = status === "reconnecting" && retryInMs ? `retrying in ${Math.round(retryInMs / 1000)}s` : status;
    if (status === "online") startVideo();
    else if (status === "denied") {
      stopVideo("Not connected");
      store.del("balbot.token");
      showLogin(httpStatus === 429 ? "Too many failed attempts. Wait a minute and try again." : "That token was not accepted.");
    } else stopVideo(status === "connecting" ? "Connecting…" : "Connection lost, retrying…");
    if (status !== "online") { robot.state = "UNKNOWN"; refreshState(); }
    refreshButtons();
  });
  link.on("hello", (m) => { app.dataset.role = m.role; refreshButtons(); });
  link.on("state", (m) => {
    robot = { state: m.state, driver: m.driver, faults: m.fault_names ?? [], m4f_alive: m.m4f_alive, vbat: m.vbat };
    refreshState();
  });
  link.on("tlm", (m) => { tlm = m; tlmDirty = true; pitchRing.push(m.pitch); });
  link.on("rtt", (ms) => { $("v-rtt").textContent = `${Math.round(ms)} ms`; $("chip-rtt").classList.toggle("warn", ms > 250); });
  link.on("lease", ({ leased, reason }) => {
    if (!leased) controller.clear();
    refreshButtons();
    const text = { busy: "Someone else is driving", role: "This token cannot drive", expired: "Control timed out",
      lost: "Control was taken by another device", preempted: null, granted: null, already: null, released: null,
      disconnected: null, not_holder: null }[reason];
    if (text) toast(text);
  });
  link.on("err", (m) => { if (m.code !== "NOT_DRIVER") toast(`${m.code}: ${m.msg}`); });
  link.connect();
}

function showLogin(error) {
  const dlg = $("login");
  $("login-error").textContent = error ?? "";
  $("login-error").hidden = !error;
  $("token").value = "";
  if (!dlg.open) dlg.showModal();
  $("token").focus();
}

$("login-form").addEventListener("submit", (e) => {
  e.preventDefault();
  const tok = $("token").value.trim();
  if (!tok) return;
  $("login").close();
  start(tok);
});

// ---- buttons ------------------------------------------------------------------------------------------------
function toggleControl() {
  if (!link) return;
  if (link.leased) { controller.clear(); link.releaseLease(); } else link.requestLease();
}
$("btn-control").addEventListener("click", toggleControl);
$("btn-arm").addEventListener("click", () => link.arm());
$("btn-disarm").addEventListener("click", () => link.disarm());
$("btn-reset").addEventListener("click", () => link.reset());
$("btn-estop").addEventListener("click", () => { if (link) { link.estop(); toast("Emergency stop sent"); } });
$("btn-boost").addEventListener("click", (e) => {
  const on = e.currentTarget.getAttribute("aria-pressed") !== "true";
  e.currentTarget.setAttribute("aria-pressed", String(on));
  if (controller) controller.boostButton = on;
});
$("btn-settings").addEventListener("click", () => $("settings").showModal());
$("btn-forget").addEventListener("click", () => {
  store.del("balbot.token");
  if (link) link.close();
  $("settings").close();
  showLogin("");
});
$("video-fps").value = pref.get("balbot.fps", "30");
$("video-fps").addEventListener("change", () => { pref.set("balbot.fps", $("video-fps").value); if (online()) startVideo(); });

// ---- sticks ----------------------------------------------------------------------------------------------------
function setupStick(el, part, axisFn, vertical) {
  const knob = el.querySelector(".knob");
  let pid = null;
  let cx = 0;
  let cy = 0;
  let radius = 1;
  function move(e) {
    const dx = e.clientX - cx;
    const dy = e.clientY - cy;
    controller?.setStick(part, axisFn(vertical ? dy : dx, radius));
    const off = clamp(vertical ? dy : dx, -radius, radius);
    knob.style.transform = vertical ? `translateY(${off}px)` : `translateX(${off}px)`;
  }
  function end(e) {
    if (e.pointerId !== pid) return;
    pid = null;
    el.classList.remove("active");
    knob.style.transform = "";
    controller?.setStick(part, 0);
  }
  el.addEventListener("pointerdown", (e) => {
    if (pid !== null) return;
    pid = e.pointerId;
    el.setPointerCapture(pid);
    const r = el.getBoundingClientRect();
    cx = r.left + r.width / 2;
    cy = r.top + r.height / 2;
    radius = (r.width / 2) * 0.62;
    el.classList.add("active");
    move(e);
    e.preventDefault();
  });
  el.addEventListener("pointermove", (e) => { if (e.pointerId === pid) move(e); });
  for (const ev of ["pointerup", "pointercancel", "lostpointercapture"]) el.addEventListener(ev, end);
}
setupStick($("stick-speed"), "speed", (d, r) => speedFromDrag(d, r), true);
setupStick($("stick-turn"), "turn", (d, r) => yawFromDrag(d, r), false);

// ---- keyboard, gamepad, safety --------------------------------------------------------------------------------
const DRIVE_KEYS = new Set(["KeyW", "KeyA", "KeyS", "KeyD", "ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight", "ShiftLeft", "ShiftRight"]);
const typing = (t) => t && (t.tagName === "INPUT" || t.tagName === "SELECT" || t.tagName === "TEXTAREA");
const dialogOpen = () => $("login").open || $("settings").open;

window.addEventListener("keydown", (e) => {
  if (typing(e.target) || dialogOpen() || e.ctrlKey || e.metaKey || e.altKey) return;
  if (e.code === "Space") {
    e.preventDefault();
    if (!e.repeat && link) { link.estop(); toast("Emergency stop sent"); }
  } else if (e.code === "Escape") {
    if (link?.leased) toggleControl();
  } else if (DRIVE_KEYS.has(e.code)) {
    e.preventDefault();
    controller?.keyDown(e.code);
  }
});
window.addEventListener("keyup", (e) => controller?.keyUp(e.code));
window.addEventListener("blur", () => controller?.clear());
document.addEventListener("visibilitychange", () => {
  if (document.hidden && link) { // never keep driving from a hidden tab
    controller.clear();
    if (link.leased) link.releaseLease();
  }
});

let prevButtons = [];
function pollGamepad() {
  const pads = navigator.getGamepads ? [...navigator.getGamepads()] : [];
  const pad = pads.find((p) => p && p.connected) ?? null;
  controller?.setPad(pad);
  const pressed = (i) => !!(pad && pad.buttons[i] && pad.buttons[i].pressed);
  const edge = (i) => pressed(i) && !prevButtons[i];
  if (pad && link) {
    if (edge(1)) { link.estop(); toast("Emergency stop sent"); }
    if (edge(2)) toggleControl();
  }
  prevButtons = pad ? pad.buttons.map((b) => b.pressed) : [];
}
setInterval(() => { pollGamepad(); controller?.update(); }, 20);

// ---- telemetry rendering ----------------------------------------------------------------------------------------
const sparkCtx = $("spark").getContext("2d");
const css = getComputedStyle(document.documentElement);
function render() {
  if (tlmDirty && tlm) {
    tlmDirty = false;
    $("t-pitch").textContent = tlm.pitch.toFixed(1);
    $("t-v").textContent = tlm.v.toFixed(2);
    $("t-yaw").textContent = tlm.yaw_rate.toFixed(0);
    $("t-u").textContent = ((tlm.uL + tlm.uR) / 2).toFixed(1);
    drawSpark(sparkCtx, pitchRing.toArray(), {
      width: 240, height: 56, range: 15, color: css.getPropertyValue("--accent").trim() || "#4aa3ff",
      zeroColor: css.getPropertyValue("--line").trim() || "#2b3441",
    });
  }
  requestAnimationFrame(render);
}
requestAnimationFrame(render);

// ---- boot --------------------------------------------------------------------------------------------------------------
(function boot() {
  const fromHash = new URLSearchParams(location.hash.slice(1)).get("token");
  if (fromHash) history.replaceState(null, "", location.pathname + location.search); // keep it out of history
  const tok = fromHash || store.get("balbot.token");
  refreshButtons();
  if (tok) start(tok);
  else showLogin("");
})();

// test hook: lets the browser tests read internal state without scraping the DOM
window.__balbot = { get link() { return link; }, get controller() { return controller; } };
