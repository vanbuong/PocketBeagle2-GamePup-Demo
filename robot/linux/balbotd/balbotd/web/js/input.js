// SPDX-License-Identifier: GPL-2.0-only
// Input mapping. Conventions match the robot: vx > 0 = forward, wz > 0 = turn LEFT (counter-clockwise).
// Screen y grows downwards and x to the right, so up = forward and left = positive yaw.

export const clamp = (v, lo = -1, hi = 1) => Math.min(hi, Math.max(lo, v));

/** Rescaling deadzone: 0 inside `dz`, then a linear ramp reaching +-1 at full deflection (no jump at the edge). */
export function deadzone(v, dz = 0.08) {
  const a = Math.abs(v);
  if (!Number.isFinite(a) || a <= dz) return 0;
  return Math.sign(v) * Math.min(1, (a - dz) / (1 - dz));
}

/** Drag offset in pixels (+up) to a speed axis. */
export const speedFromDrag = (dyPx, radiusPx, dz = 0.08) => deadzone(clamp(-dyPx / radiusPx), dz);
/** Drag offset in pixels (+right) to a yaw axis: dragging LEFT turns left (positive). */
export const yawFromDrag = (dxPx, radiusPx, dz = 0.08) => deadzone(clamp(-dxPx / radiusPx), dz);

export function keyboardAxes(down) {
  const has = (...codes) => codes.some((c) => down.has(c));
  const vx = (has("KeyW", "ArrowUp") ? 1 : 0) - (has("KeyS", "ArrowDown") ? 1 : 0);
  const wz = (has("KeyA", "ArrowLeft") ? 1 : 0) - (has("KeyD", "ArrowRight") ? 1 : 0);
  return { vx, wz, boost: has("ShiftLeft", "ShiftRight") };
}

/** Standard-mapping gamepad: left stick Y -> speed, right stick X -> yaw. */
export function gamepadAxes(pad, dz = 0.12) {
  if (!pad || !pad.axes || pad.axes.length < 4) return { vx: 0, wz: 0 };
  return { vx: deadzone(-pad.axes[1], dz), wz: deadzone(-pad.axes[2], dz) };
}

/** Per axis the source with the largest magnitude wins, so a released stick never cancels a held key. */
export function mixAxes(...sources) {
  let vx = 0;
  let wz = 0;
  for (const s of sources) {
    if (Math.abs(s.vx) > Math.abs(vx)) vx = s.vx;
    if (Math.abs(s.wz) > Math.abs(wz)) wz = s.wz;
  }
  return { vx: clamp(vx), wz: clamp(wz) };
}
