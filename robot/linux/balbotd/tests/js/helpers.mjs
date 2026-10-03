// SPDX-License-Identifier: GPL-2.0-only
// Fakes for link tests: a manual clock with timers, a scriptable WebSocket and fetch.
export class FakeClock {
  constructor() { this.t = 1000; this.nextId = 1; this.items = new Map(); }
  now = () => this.t;
  setInterval = (fn, ms) => { const id = this.nextId++; this.items.set(id, { fn, ms, due: this.t + ms, every: true }); return id; };
  setTimeout = (fn, ms) => { const id = this.nextId++; this.items.set(id, { fn, ms, due: this.t + ms, every: false }); return id; };
  clearInterval = (id) => { this.items.delete(id); };
  clearTimeout = (id) => { this.items.delete(id); };
  get timers() { return { setInterval: this.setInterval, clearInterval: this.clearInterval, setTimeout: this.setTimeout, clearTimeout: this.clearTimeout }; }
  advance(ms) {
    const end = this.t + ms;
    for (;;) {
      let nextId = null;
      let nextDue = Infinity;
      for (const [id, it] of this.items) if (it.due <= end && it.due < nextDue) { nextId = id; nextDue = it.due; }
      if (nextId === null) break;
      const it = this.items.get(nextId);
      this.t = it.due;
      if (it.every) it.due += it.ms; else this.items.delete(nextId);
      it.fn();
    }
    this.t = end;
  }
}

export class FakeWebSocket {
  static instances = [];
  constructor(url) { this.url = url; this.readyState = 0; this.sent = []; FakeWebSocket.instances.push(this); }
  send(data) { if (this.readyState !== 1) throw new Error("send on a socket that is not open"); this.sent.push(JSON.parse(data)); }
  close() { if (this.readyState === 3) return; this.readyState = 3; this.onclose?.({}); }
  // test controls
  open() { this.readyState = 1; this.onopen?.({}); }
  receive(obj) { this.onmessage?.({ data: JSON.stringify(obj) }); }
  drop() { this.readyState = 3; this.onclose?.({}); }
  of(type) { return this.sent.filter((m) => m.t === type); }
}

export const okFetch = (status = 200) => async () => ({ status, ok: status >= 200 && status < 300 });

export async function connected(RobotLink, { status = 200, role = "driver" } = {}) {
  FakeWebSocket.instances.length = 0;
  const clock = new FakeClock();
  const link = new RobotLink({ baseUrl: "http://robot.local:8080", token: "tok/en", WebSocketCtor: FakeWebSocket,
    fetchImpl: okFetch(status), timers: clock.timers, now: clock.now });
  await link.connect();
  const ws = FakeWebSocket.instances.at(-1);
  if (ws) { ws.open(); ws.receive({ t: "hello", proto: 1, role }); }
  return { link, clock, ws };
}
