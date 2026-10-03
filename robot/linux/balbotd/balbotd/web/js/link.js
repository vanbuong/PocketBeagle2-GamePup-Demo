// SPDX-License-Identifier: GPL-2.0-only
// WebSocket link to balbotd: authentication check, reconnect with backoff, 1 Hz ping (RTT + lease keep-alive),
// 50 Hz drive loop while holding the lease, and the client-side safety rules of doc 03, 3.6:
//  - never drive without the lease; a reconnect does NOT re-take it
//  - an intent that is not refreshed for 500 ms (lost pointer, stuck key) is treated as zero
//  - E-STOP is sent three times, 15 ms apart
import { msg, parseMessage, seqNext, FLAG_BALANCE_ON, FLAG_BOOST } from "./protocol.js";

export const DRIVE_PERIOD_MS = 20;
export const PING_PERIOD_MS = 1000;
export const INTENT_TIMEOUT_MS = 500;
export const BACKOFF_MS = [500, 1000, 2000, 4000, 5000];

export class RobotLink {
  constructor({ baseUrl, token, WebSocketCtor, fetchImpl, timers, now, subRate = 20 }) {
    this.baseUrl = baseUrl;
    this.token = token;
    this.WS = WebSocketCtor;
    this.fetch = fetchImpl;
    this.timers = timers;
    this.now = now;
    this.subRate = subRate;
    this.handlers = new Map();
    this.ws = null;
    this.status = "offline"; // offline | connecting | online | reconnecting | denied
    this.role = null;
    this.leased = false;
    this.seq = 0;
    this.intent = { vx: 0, wz: 0, boost: false, t: -Infinity };
    this.attempt = 0;
    this.closing = false;
    this.driveTimer = this.pingTimer = this.retryTimer = null;
    this.rtt = null;
    this.stats = { driveSent: 0, reconnects: 0 };
  }

  on(name, fn) {
    if (!this.handlers.has(name)) this.handlers.set(name, []);
    this.handlers.get(name).push(fn);
    return this;
  }

  emit(name, arg) {
    for (const fn of this.handlers.get(name) ?? []) fn(arg);
  }

  setStatus(s, extra) {
    this.status = s;
    this.emit("status", { status: s, ...extra });
  }

  get wsUrl() {
    const u = new URL(this.baseUrl);
    u.protocol = u.protocol === "https:" ? "wss:" : "ws:";
    u.pathname = "/ws";
    u.search = "token=" + encodeURIComponent(this.token);
    return u.toString();
  }

  /** Check the token over HTTP first: a failed WebSocket handshake cannot tell a bad token from a dead network. */
  async connect() {
    this.closing = false;
    this.setStatus(this.attempt ? "reconnecting" : "connecting");
    let res;
    try {
      res = await this.fetch(new URL("/api/v1/state", this.baseUrl).toString(),
        { headers: { Authorization: "Bearer " + this.token } });
    } catch {
      return this.scheduleRetry();
    }
    if (res.status === 401 || res.status === 403 || res.status === 429) {
      this.setStatus("denied", { httpStatus: res.status });
      return undefined; // never retry a refused token: it would only feed the server's brute-force throttle
    }
    if (!res.ok) return this.scheduleRetry();
    this.open();
    return undefined;
  }

  open() {
    const ws = new this.WS(this.wsUrl);
    this.ws = ws;
    ws.onopen = () => {
      this.attempt = 0;
      this.setStatus("online");
      this.send(msg.sub(this.subRate));
      this.sendPing();
      this.pingTimer = this.timers.setInterval(() => this.sendPing(), PING_PERIOD_MS);
      this.driveTimer = this.timers.setInterval(() => this.driveTick(), DRIVE_PERIOD_MS);
    };
    ws.onmessage = (ev) => this.onMessage(typeof ev.data === "string" ? ev.data : "");
    ws.onclose = () => this.onClose(ws);
    ws.onerror = () => {}; // always followed by onclose
  }

  onClose(ws) {
    if (ws !== this.ws) return;
    this.ws = null;
    this.timers.clearInterval(this.driveTimer);
    this.timers.clearInterval(this.pingTimer);
    this.driveTimer = this.pingTimer = null;
    this.leased = false; // an explicit new request is required after any reconnect
    this.emit("lease", { leased: false, reason: "disconnected" });
    if (this.closing) return this.setStatus("offline");
    return this.scheduleRetry();
  }

  scheduleRetry() {
    const delay = BACKOFF_MS[Math.min(this.attempt, BACKOFF_MS.length - 1)];
    this.attempt += 1;
    this.stats.reconnects += 1;
    this.setStatus("reconnecting", { retryInMs: delay });
    this.retryTimer = this.timers.setTimeout(() => this.connect(), delay);
  }

  close() {
    this.closing = true;
    this.timers.clearTimeout(this.retryTimer);
    if (this.ws) this.ws.close();
    else this.setStatus("offline");
  }

  send(obj) {
    if (!this.ws || this.ws.readyState !== 1) return false;
    this.ws.send(JSON.stringify(obj));
    return true;
  }

  sendPing() {
    this.send(msg.ping(this.now()));
  }

  onMessage(text) {
    const m = parseMessage(text);
    if (!m) return;
    switch (m.t) {
      case "hello":
        this.role = m.role;
        break;
      case "lease":
        this.leased = m.granted === true;
        this.emit("lease", { leased: this.leased, reason: m.reason, epoch: m.epoch });
        break;
      case "pong":
        if (typeof m.ts === "number") {
          this.rtt = Math.max(0, this.now() - m.ts);
          this.emit("rtt", this.rtt);
        }
        break;
      case "err":
        if (m.code === "NOT_DRIVER" && this.leased) { // preempted or expired while we were still sending
          this.leased = false;
          this.emit("lease", { leased: false, reason: "lost" });
        }
        break;
      default:
        break;
    }
    this.emit(m.t, m);
  }

  // ---- control ----------------------------------------------------------------------------------------
  requestLease() { return this.send(msg.lease("request")); }
  releaseLease() {
    this.setIntent(0, 0, false);
    return this.send(msg.lease("release"));
  }
  arm() { return this.send(msg.arm()); }
  disarm() { return this.send(msg.disarm()); }
  reset() { return this.send(msg.reset()); }

  estop() {
    this.setIntent(0, 0, false);
    const sent = this.send(msg.estop());
    for (const d of [15, 30]) this.timers.setTimeout(() => this.send(msg.estop()), d);
    return sent;
  }

  setIntent(vx, wz, boost = false) {
    this.intent = { vx, wz, boost, t: this.now() };
  }

  driveTick() {
    if (!this.leased) return;
    const stale = this.now() - this.intent.t > INTENT_TIMEOUT_MS;
    const { vx, wz, boost } = stale ? { vx: 0, wz: 0, boost: false } : this.intent;
    this.seq = seqNext(this.seq);
    const flags = FLAG_BALANCE_ON | (boost ? FLAG_BOOST : 0);
    if (this.send(msg.drive(this.seq, this.now(), vx, wz, flags))) this.stats.driveSent += 1;
  }
}
