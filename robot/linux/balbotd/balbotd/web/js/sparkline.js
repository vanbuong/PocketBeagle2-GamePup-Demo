// SPDX-License-Identifier: GPL-2.0-only
// Fixed-size ring of samples and a minimal canvas line plot (pitch history).

export class Ring {
  constructor(size) {
    this.size = size;
    this.buf = new Float32Array(size);
    this.n = 0;
    this.head = 0;
  }

  push(v) {
    this.buf[this.head] = v;
    this.head = (this.head + 1) % this.size;
    this.n = Math.min(this.n + 1, this.size);
  }

  /** Samples oldest to newest. */
  toArray() {
    const out = new Array(this.n);
    const start = (this.head - this.n + this.size) % this.size;
    for (let i = 0; i < this.n; i++) out[i] = this.buf[(start + i) % this.size];
    return out;
  }
}

export function drawSpark(ctx, values, { width, height, range, color, zeroColor }) {
  ctx.clearRect(0, 0, width, height);
  const mid = height / 2;
  ctx.strokeStyle = zeroColor;
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(0, mid);
  ctx.lineTo(width, mid);
  ctx.stroke();
  if (values.length < 2) return;
  ctx.strokeStyle = color;
  ctx.lineWidth = 1.5;
  ctx.beginPath();
  values.forEach((v, i) => {
    const x = (i / (values.length - 1)) * width;
    const y = mid - Math.max(-range, Math.min(range, v)) / range * (mid - 2);
    if (i === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  });
  ctx.stroke();
}
