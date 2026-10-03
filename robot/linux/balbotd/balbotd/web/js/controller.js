// SPDX-License-Identifier: GPL-2.0-only
// Combines the input sources into one intent for the link. Pure logic, no DOM.
import { gamepadAxes, keyboardAxes, mixAxes } from "./input.js";

export class Controller {
  constructor(link) {
    this.link = link;
    this.stick = { vx: 0, wz: 0 };
    this.keys = new Set();
    this.pad = null;
    this.boostButton = false;
  }

  setStick(part, value) {
    if (part === "speed") this.stick.vx = value;
    else if (part === "turn") this.stick.wz = value;
  }

  setPad(pad) { this.pad = pad; }
  keyDown(code) { this.keys.add(code); }
  keyUp(code) { this.keys.delete(code); }

  /** Zero everything (focus lost, tab hidden, control released). */
  clear() {
    this.stick = { vx: 0, wz: 0 };
    this.keys.clear();
    this.pad = null;
    this.link.setIntent(0, 0, false);
  }

  /** Call every frame or timer tick; refreshes the link's intent (its 500 ms watchdog needs this). */
  update() {
    const k = keyboardAxes(this.keys);
    const { vx, wz } = mixAxes(this.stick, { vx: k.vx, wz: k.wz }, gamepadAxes(this.pad));
    const boost = k.boost || this.boostButton;
    this.link.setIntent(vx, wz, boost);
    return { vx, wz, boost };
  }
}
