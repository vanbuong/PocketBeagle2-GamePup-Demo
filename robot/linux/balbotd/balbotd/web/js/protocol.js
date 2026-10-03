// SPDX-License-Identifier: GPL-2.0-only
// Message builders and parsing for the balbotd WebSocket protocol v1 (robot/linux/balbotd/README.md).

export const FLAG_BALANCE_ON = 1;
export const FLAG_BOOST = 2;
export const FLAG_HOLD = 4;

export const seqNext = (seq) => (seq + 1) & 0xffff;

export const msg = {
  lease: (action) => ({ t: "lease", action }),
  drive: (seq, ts, vx, wz, flags) => ({ t: "drive", seq, ts: Math.round(ts), vx, wz, flags }),
  arm: () => ({ t: "arm" }),
  disarm: () => ({ t: "disarm" }),
  estop: () => ({ t: "estop" }),
  reset: () => ({ t: "reset" }),
  ping: (ts) => ({ t: "ping", ts: Math.round(ts) }),
  sub: (rate) => ({ t: "sub", rate }),
};

/** Parse a server message; returns null for anything that is not an object with a string `t`. */
export function parseMessage(text) {
  let m;
  try {
    m = JSON.parse(text);
  } catch {
    return null;
  }
  return m && typeof m === "object" && !Array.isArray(m) && typeof m.t === "string" ? m : null;
}

/** States in which the motors can be driven or are about to be (used to colour the UI). */
export const STATE_KIND = {
  BOOT: "wait", CALIBRATING: "wait", STANDBY: "idle", BALANCING: "active",
  LAYING_DOWN: "warn", FALLEN: "bad", FAULT: "bad",
};

export const stateKind = (s) => STATE_KIND[s] ?? "unknown";

export const FAULT_TEXT = {
  IMU: "IMU not responding", OVERRUN: "control loop overrun", ESTOP: "emergency stop", ENCODER: "encoder fault",
  STALL: "motor stall", PRU: "PRU watchdog", CAL: "calibration failed", M4F_DEAD: "robot controller not responding",
};

export const describeFaults = (names) => (names || []).map((n) => FAULT_TEXT[n] ?? n);
