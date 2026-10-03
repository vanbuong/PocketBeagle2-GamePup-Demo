// SPDX-License-Identifier: GPL-2.0-only
import test from "node:test";
import assert from "node:assert/strict";
import { clamp, deadzone, gamepadAxes, keyboardAxes, mixAxes, speedFromDrag, yawFromDrag } from "../../balbotd/web/js/input.js";
import { Controller } from "../../balbotd/web/js/controller.js";
import { Ring } from "../../balbotd/web/js/sparkline.js";

const near = (a, b, e = 1e-9) => assert.ok(Math.abs(a - b) <= e, `${a} vs ${b}`);

test("deadzone: zero inside, continuous at the edge, +-1 at full deflection, odd symmetric", () => {
  assert.equal(deadzone(0.05, 0.1), 0);
  assert.equal(deadzone(-0.1, 0.1), 0);
  near(deadzone(0.1001, 0.1), 0.0001 / 0.9, 1e-6);
  assert.equal(deadzone(1, 0.1), 1);
  assert.equal(deadzone(5, 0.1), 1);
  for (const v of [0.2, 0.5, 0.9]) assert.equal(deadzone(v, 0.1), -deadzone(-v, 0.1));
  assert.equal(deadzone(NaN), 0);
  assert.equal(deadzone(Infinity), 0);
});

test("dragging up drives forward, down reverses", () => {
  assert.ok(speedFromDrag(-60, 60) === 1);
  assert.ok(speedFromDrag(-30, 60) > 0 && speedFromDrag(-30, 60) < 1);
  assert.ok(speedFromDrag(60, 60) === -1);
  assert.equal(speedFromDrag(3, 60), 0);
  assert.equal(speedFromDrag(-500, 60), 1); // clamped, never above full stick
});

test("dragging left turns left (positive yaw), right turns right", () => {
  assert.ok(yawFromDrag(-60, 60) === 1);
  assert.ok(yawFromDrag(60, 60) === -1);
  assert.equal(yawFromDrag(0, 60), 0);
});

test("keyboard: WASD and arrows, opposite keys cancel, shift is boost", () => {
  assert.deepEqual(keyboardAxes(new Set(["KeyW"])), { vx: 1, wz: 0, boost: false });
  assert.deepEqual(keyboardAxes(new Set(["ArrowDown", "KeyA"])), { vx: -1, wz: 1, boost: false });
  assert.deepEqual(keyboardAxes(new Set(["KeyD"])), { vx: 0, wz: -1, boost: false });
  assert.deepEqual(keyboardAxes(new Set(["KeyW", "KeyS", "KeyA", "KeyD"])), { vx: 0, wz: 0, boost: false });
  assert.equal(keyboardAxes(new Set(["ShiftLeft"])).boost, true);
  assert.deepEqual(keyboardAxes(new Set()), { vx: 0, wz: 0, boost: false });
});

test("gamepad: left stick Y is speed (up = forward), right stick X is yaw (left = positive)", () => {
  const pad = (a) => ({ axes: a });
  assert.ok(gamepadAxes(pad([0, -1, 0, 0])).vx === 1);
  assert.ok(gamepadAxes(pad([0, 1, 0, 0])).vx === -1);
  assert.ok(gamepadAxes(pad([0, 0, -1, 0])).wz === 1);
  assert.deepEqual(gamepadAxes(pad([0, 0.05, 0.05, 0])), { vx: 0, wz: 0 }); // drift inside the deadzone
  assert.deepEqual(gamepadAxes(null), { vx: 0, wz: 0 });
  assert.deepEqual(gamepadAxes(pad([0, 1])), { vx: 0, wz: 0 }); // not a standard mapping
});

test("mixAxes: the largest magnitude per axis wins, a released source never cancels a held one", () => {
  assert.deepEqual(mixAxes({ vx: 0, wz: 0 }, { vx: 1, wz: 0 }), { vx: 1, wz: 0 });
  assert.deepEqual(mixAxes({ vx: 0.3, wz: -0.8 }, { vx: -0.6, wz: 0.2 }), { vx: -0.6, wz: -0.8 });
  assert.deepEqual(mixAxes({ vx: 7, wz: -7 }), { vx: 1, wz: -1 });
  assert.deepEqual(mixAxes(), { vx: 0, wz: 0 });
});

test("clamp", () => { assert.equal(clamp(2), 1); assert.equal(clamp(-2), -1); assert.equal(clamp(0.3), 0.3); });

test("Controller mixes stick, keyboard and gamepad into the link's intent and clears on demand", () => {
  const seen = [];
  const link = { setIntent: (vx, wz, boost) => seen.push({ vx, wz, boost }) };
  const c = new Controller(link);
  c.setStick("speed", 0.5);
  c.keyDown("KeyA");
  c.keyDown("ShiftLeft");
  assert.deepEqual(c.update(), { vx: 0.5, wz: 1, boost: true });
  c.setPad({ axes: [0, -1, 0, 0] });
  assert.equal(c.update().vx, 1);
  c.boostButton = true;
  c.keyUp("ShiftLeft");
  assert.equal(c.update().boost, true);
  c.clear();
  assert.deepEqual(seen.at(-1), { vx: 0, wz: 0, boost: false });
  assert.deepEqual(c.update(), { vx: 0, wz: 0, boost: true }); // boost button is a user setting, not an input
});

test("Ring keeps the newest samples in order", () => {
  const r = new Ring(4);
  assert.deepEqual(r.toArray(), []);
  [1, 2, 3].forEach((v) => r.push(v));
  assert.deepEqual(r.toArray(), [1, 2, 3]);
  [4, 5, 6].forEach((v) => r.push(v));
  assert.deepEqual(r.toArray(), [3, 4, 5, 6]);
});
