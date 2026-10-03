// SPDX-License-Identifier: GPL-2.0-only
import test from "node:test";
import assert from "node:assert/strict";
import { RobotLink, BACKOFF_MS, DRIVE_PERIOD_MS, INTENT_TIMEOUT_MS } from "../../balbotd/web/js/link.js";
import { describeFaults, msg, parseMessage, seqNext, stateKind } from "../../balbotd/web/js/protocol.js";
import { FakeClock, FakeWebSocket, connected, okFetch } from "./helpers.mjs";

test("protocol: seq wraps at 16 bits, builders have the documented fields", () => {
  assert.equal(seqNext(65535), 0);
  assert.equal(seqNext(7), 8);
  assert.deepEqual(msg.drive(1, 12.6, 0.5, -0.5, 3), { t: "drive", seq: 1, ts: 13, vx: 0.5, wz: -0.5, flags: 3 });
  assert.deepEqual(msg.lease("request"), { t: "lease", action: "request" });
  assert.deepEqual(msg.reset(), { t: "reset" });
});

test("protocol: parseMessage accepts objects with a string t only", () => {
  assert.deepEqual(parseMessage('{"t":"state","x":1}'), { t: "state", x: 1 });
  for (const bad of ["nope", "[1]", "null", '{"t":5}', '{"x":1}', "", "42"]) assert.equal(parseMessage(bad), null);
  assert.equal(stateKind("BALANCING"), "active");
  assert.equal(stateKind("???"), "unknown");
  assert.deepEqual(describeFaults(["ESTOP", "WEIRD"]), ["emergency stop", "WEIRD"]);
  assert.deepEqual(describeFaults(undefined), []);
});

test("connect: token is checked over HTTP first, then the WebSocket opens with it in the URL", async () => {
  const { link, ws } = await connected(RobotLink);
  assert.equal(link.status, "online");
  assert.equal(ws.url, "ws://robot.local:8080/ws?token=tok%2Fen");
  assert.deepEqual(ws.of("sub"), [{ t: "sub", rate: 20 }]);
  assert.equal(ws.of("ping").length, 1);
  assert.equal(link.role, "driver");
});

test("a refused token is never retried (it would only feed the server's brute-force throttle)", async () => {
  for (const status of [401, 403, 429]) {
    FakeWebSocket.instances.length = 0;
    const clock = new FakeClock();
    const link = new RobotLink({ baseUrl: "http://x", token: "t", WebSocketCtor: FakeWebSocket, fetchImpl: okFetch(status), timers: clock.timers, now: clock.now });
    const seen = [];
    link.on("status", (s) => seen.push(s));
    await link.connect();
    clock.advance(60000);
    assert.equal(link.status, "denied");
    assert.equal(seen.at(-1).httpStatus, status);
    assert.equal(FakeWebSocket.instances.length, 0);
  }
});

test("network failure retries with growing backoff, capped, and resets after a successful connection", async () => {
  FakeWebSocket.instances.length = 0;
  const clock = new FakeClock();
  let fail = true;
  const fetchImpl = async () => { if (fail) throw new Error("down"); return { status: 200, ok: true }; };
  const link = new RobotLink({ baseUrl: "http://x", token: "t", WebSocketCtor: FakeWebSocket, fetchImpl, timers: clock.timers, now: clock.now });
  const retries = [];
  link.on("status", (s) => { if (s.retryInMs) retries.push(s.retryInMs); });
  await link.connect();
  for (let i = 0; i < 7; i++) { clock.advance(6000); await Promise.resolve(); await Promise.resolve(); }
  assert.deepEqual(retries.slice(0, 5), BACKOFF_MS);
  assert.ok(retries.slice(5).every((r) => r === BACKOFF_MS.at(-1)));
  fail = false;
  clock.advance(6000);
  await new Promise((r) => setImmediate(r));
  FakeWebSocket.instances.at(-1).open();
  assert.equal(link.status, "online");
  assert.equal(link.attempt, 0);
});

test("ws close: reconnects, and the lease is NOT re-taken automatically", async () => {
  const { link, clock, ws } = await connected(RobotLink);
  ws.receive({ t: "lease", granted: true, epoch: 1, reason: "granted" });
  assert.equal(link.leased, true);
  ws.drop();
  assert.equal(link.leased, false);
  assert.equal(link.status, "reconnecting");
  clock.advance(BACKOFF_MS[0] + 1);
  await new Promise((r) => setImmediate(r));
  const ws2 = FakeWebSocket.instances.at(-1);
  assert.notEqual(ws2, ws);
  ws2.open();
  clock.advance(1000);
  assert.equal(ws2.of("lease").length, 0, "reconnect must not request the lease");
  assert.equal(ws2.of("drive").length, 0, "and must not drive");
});

test("drive loop: 50 Hz only while holding the lease, with increasing seq and the boost flag", async () => {
  const { link, clock, ws } = await connected(RobotLink);
  clock.advance(500);
  assert.equal(ws.of("drive").length, 0, "no lease, no drive");
  ws.receive({ t: "lease", granted: true, epoch: 1, reason: "granted" });
  link.setIntent(0.5, -0.25, false);
  clock.advance(100);
  const drives = ws.of("drive");
  assert.equal(drives.length, 100 / DRIVE_PERIOD_MS);
  assert.deepEqual(drives.map((d) => d.seq), [1, 2, 3, 4, 5]);
  assert.ok(drives.every((d) => d.vx === 0.5 && d.wz === -0.25 && d.flags === 1));
  link.setIntent(1, 0, true);
  clock.advance(DRIVE_PERIOD_MS);
  assert.equal(ws.of("drive").at(-1).flags, 3);
});

test("a stale intent (stuck pointer or key) is sent as zero after 500 ms", async () => {
  const { link, clock, ws } = await connected(RobotLink);
  ws.receive({ t: "lease", granted: true, epoch: 1, reason: "granted" });
  link.setIntent(1, 1, true);
  clock.advance(INTENT_TIMEOUT_MS - 40);
  assert.equal(ws.of("drive").at(-1).vx, 1);
  clock.advance(100);
  const last = ws.of("drive").at(-1);
  assert.deepEqual([last.vx, last.wz, last.flags], [0, 0, 1]);
  link.setIntent(0.7, 0, false); // refreshed: moves again
  clock.advance(DRIVE_PERIOD_MS);
  assert.equal(ws.of("drive").at(-1).vx, 0.7);
});

test("seq wraps around 65535 in the drive stream", async () => {
  const { link, clock, ws } = await connected(RobotLink);
  ws.receive({ t: "lease", granted: true, epoch: 1, reason: "granted" });
  link.seq = 65533;
  link.setIntent(0.1, 0, false);
  clock.advance(DRIVE_PERIOD_MS * 4);
  assert.deepEqual(ws.of("drive").map((d) => d.seq), [65534, 65535, 0, 1]);
});

test("lease lifecycle: release zeroes the intent first; denial, expiry and preemption stop driving", async () => {
  const { link, clock, ws } = await connected(RobotLink);
  const events = [];
  link.on("lease", (e) => events.push(e));
  link.requestLease();
  assert.deepEqual(ws.of("lease").at(-1), { t: "lease", action: "request" });
  ws.receive({ t: "lease", granted: false, epoch: 0, reason: "busy" });
  assert.equal(link.leased, false);
  ws.receive({ t: "lease", granted: true, epoch: 2, reason: "granted" });
  link.setIntent(1, 0, false);
  link.releaseLease();
  assert.equal(link.intent.vx, 0);
  assert.deepEqual(ws.of("lease").at(-1), { t: "lease", action: "release" });
  ws.receive({ t: "lease", granted: false, epoch: 0, reason: "released" });
  clock.advance(200);
  const n = ws.of("drive").length;
  clock.advance(200);
  assert.equal(ws.of("drive").length, n, "no drive after release");
  ws.receive({ t: "lease", granted: true, epoch: 3, reason: "granted" });
  clock.advance(60);
  assert.ok(ws.of("drive").length > n);
  ws.receive({ t: "err", code: "NOT_DRIVER", msg: "request the lease first" }); // someone preempted us
  assert.equal(link.leased, false);
  assert.equal(events.at(-1).reason, "lost");
  const m = ws.of("drive").length;
  clock.advance(200);
  assert.equal(ws.of("drive").length, m);
  ws.receive({ t: "lease", granted: false, epoch: 0, reason: "expired" });
  assert.equal(link.leased, false);
});

test("E-STOP: sent immediately and twice more, intent zeroed, works without the lease", async () => {
  const { link, clock, ws } = await connected(RobotLink, { role: "viewer" });
  link.setIntent(1, 1, true);
  assert.equal(link.estop(), true);
  assert.equal(ws.of("estop").length, 1);
  assert.equal(link.intent.vx, 0);
  clock.advance(14);
  assert.equal(ws.of("estop").length, 1);
  clock.advance(20);
  assert.equal(ws.of("estop").length, 3);
});

test("send never throws when the socket is not open", async () => {
  const { link, ws } = await connected(RobotLink);
  ws.readyState = 3;
  assert.equal(link.send({ t: "ping", ts: 1 }), false);
  assert.equal(link.estop(), false);
});

test("ping measures the round trip and doubles as the lease keep-alive", async () => {
  const { link, clock, ws } = await connected(RobotLink);
  const rtts = [];
  link.on("rtt", (v) => rtts.push(v));
  clock.advance(1000);
  assert.equal(ws.of("ping").length, 2); // one at open, one per second after
  const sent = ws.of("ping").at(-1);
  clock.advance(37);
  ws.receive({ t: "pong", ts: sent.ts, srv: 1 });
  assert.equal(rtts.length, 1);
  assert.ok(rtts[0] >= 36 && rtts[0] <= 38);
});

test("garbage from the server is ignored, close() stops everything", async () => {
  const { link, clock, ws } = await connected(RobotLink);
  ws.onmessage({ data: "not json" });
  ws.onmessage({ data: "[1,2]" });
  ws.onmessage({ data: 5 });
  assert.equal(link.status, "online");
  link.close();
  assert.equal(link.status, "offline");
  clock.advance(10000);
  assert.equal(FakeWebSocket.instances.length, 1, "no reconnect after an explicit close");
});

test("a late close event from an old socket does not disturb the new one", async () => {
  const { link, clock, ws } = await connected(RobotLink);
  ws.drop();
  clock.advance(BACKOFF_MS[0] + 1);
  await new Promise((r) => setImmediate(r));
  const ws2 = FakeWebSocket.instances.at(-1);
  ws2.open();
  ws.onclose({}); // stale event
  assert.equal(link.status, "online");
  assert.equal(link.ws, ws2);
});
