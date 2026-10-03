# SPDX-License-Identifier: GPL-2.0-only
"""A behavioural stand-in for the M4F firmware, speaking the real rpmsg frames.

It is NOT the firmware: pitch and speed come from a trivial kinematic stub, and the state machine is a Python
re-implementation of the rules in core/src/state.c (tip-over, arming window, link-loss lay-down, E-stop,
low battery). It exists so balbotd can be developed and tested without hardware, including failure cases:
hang(), tip_over(), raise_fault(), set_vbat(), a lossy/corrupting link.
"""
from __future__ import annotations

import asyncio
import math
import struct
import time
from dataclasses import dataclass
from typing import Optional

from . import cfgkeys
from . import protocol as P
from .link import Link, LinkClosed


@dataclass
class FakeConfig:
    tick_s: float = 0.005
    tlm_period_s: float = 0.010
    evt_period_s: float = 1.0
    boot_s: float = 0.05
    cal_s: float = 0.20
    arm_hold_s: float = 1.0
    link_lost_s: float = 0.5        # no valid frame from balbotd: ramp velocity to zero
    laydown_after_s: float = 10.0   # ... and lie down after this long
    laydown_s: float = 1.0
    drive_stale_s: float = 0.5      # no CMD_DRIVE: target zero
    v_limit: float = 0.96           # firmware limits are authoritative (0.6 m/s x boost 1.6)
    w_limit: float = 2.5
    a_max: float = 0.8
    alpha_max: float = 6.0
    tip_deg: float = 35.0
    vbat: float = 11.1
    vbat_low: float = 9.9
    vbat_cut: float = 9.6


class FakeM4F:
    def __init__(self, link: Link, cfg: Optional[FakeConfig] = None, clock=time.monotonic):
        self.link = link
        self.cfg = cfg or FakeConfig()
        self.clock = clock
        # observable state
        self.state = P.ST_BOOT
        self.faults = 0
        self.v = 0.0
        self.w = 0.0
        self.rest_pitch_deg = 0.0
        self.vbat = self.cfg.vbat
        self.cfg_values = {k.id: k.default for k in cfgkeys.KEYS}
        # counters
        self.rx_frames = self.crc_errors = self.drive_frames = self.stale_epoch_drops = 0
        self.seq = P.SeqTracker()
        self.refused = 0
        self.saves = 0
        self.cal_results = 0
        # internals
        self._tx_seq = 0
        self._t0 = clock()
        self._state_t0 = self._t0
        self._last_rx = self._t0
        self._last_drive = self._t0
        self._link_lost_since: Optional[float] = None
        self._v_target = 0.0
        self._w_target = 0.0
        self._max_epoch = 0
        self._arm_pending = False
        self._arm_t = 0.0
        self._upright_t = 0.0
        self._disarm_seen = False
        self._pitch_override: Optional[float] = None
        self._hung = False
        self._last_tick = self._t0
        self._last_tlm = self._t0
        self._last_evt = self._t0
        self._tasks: list[asyncio.Task] = []
        self._cal_end: Optional[float] = None

    # ---- test / dev controls ----------------------------------------------------
    def hang(self) -> None:
        """Stop reading and sending, like a crashed firmware."""
        self._hung = True

    def resume(self) -> None:
        self._hung = False
        self._last_rx = self.clock()

    def tip_over(self, deg: float = 40.0) -> None:
        self._pitch_override = deg

    def right_up(self) -> None:
        self._pitch_override = None

    def raise_fault(self, bits: int) -> None:
        self._enter_fault(bits)

    def set_vbat(self, volts: float) -> None:
        self.vbat = volts

    # ---- lifecycle --------------------------------------------------------------
    def start(self) -> None:
        self._tasks = [asyncio.create_task(self._rx_loop()), asyncio.create_task(self._tick_loop())]

    async def stop(self) -> None:
        for t in self._tasks:
            t.cancel()
        await asyncio.gather(*self._tasks, return_exceptions=True)
        self._tasks = []

    # ---- helpers ---------------------------------------------------------------
    @property
    def pitch_deg(self) -> float:
        if self._pitch_override is not None:
            return self._pitch_override
        return self.rest_pitch_deg + (self._accel / 9.81) * 57.29578 if self.state == P.ST_BALANCING else self.rest_pitch_deg

    _accel = 0.0

    def _enter(self, st: int) -> None:
        self.state = st
        self._state_t0 = self.clock()
        self._arm_pending = False
        self._upright_t = 0.0
        self._disarm_seen = False

    def _enter_fault(self, bits: int) -> None:
        self.faults |= bits
        self._v_target = self._w_target = 0.0
        if self.state != P.ST_FAULT:
            self._enter(P.ST_FAULT)
            self._send_evt_nowait()

    def _motors_active(self) -> bool:
        return self.state in (P.ST_BALANCING, P.ST_LAYING_DOWN)

    async def _send(self, type_: int, payload: bytes = b"") -> None:
        if self._hung:
            return
        self._tx_seq = (self._tx_seq + 1) & 0xFFFF
        try:
            await self.link.send(P.encode(type_, self._tx_seq, payload))
        except LinkClosed:
            pass

    def _send_evt_nowait(self) -> None:
        asyncio.get_running_loop().create_task(self._send(P.EVT_STATE, P.pack_evt_state(self.state, self.faults)))

    # ---- receive ----------------------------------------------------------------
    async def _rx_loop(self) -> None:
        while True:
            try:
                data = await self.link.recv()
            except LinkClosed:
                return
            if self._hung:
                continue
            try:
                fr = P.decode(data)
            except P.ProtocolError:
                self.crc_errors += 1
                continue
            self.rx_frames += 1
            self.seq.check(fr.seq)
            self._last_rx = self.clock()
            await self._handle(fr)

    async def _handle(self, fr: P.Frame) -> None:
        t = fr.type
        try:
            if t == P.CMD_DRIVE:
                self._on_drive(*P.unpack_cmd_drive(fr.payload))
            elif t == P.CMD_HEARTBEAT:
                P.unpack_heartbeat(fr.payload)
            elif t == P.CMD_ARM:
                self._on_arm()
                await self._send(P.EVT_STATE, P.pack_evt_state(self.state, self.faults))
            elif t == P.CMD_DISARM:
                self._arm_pending = False
                if self.state in (P.ST_BALANCING, P.ST_LAYING_DOWN):
                    self._enter(P.ST_STANDBY)
                elif self.state == P.ST_FALLEN:
                    self._disarm_seen = True
                self._v_target = self._w_target = 0.0
                await self._send(P.EVT_STATE, P.pack_evt_state(self.state, self.faults))
            elif t == P.CMD_ESTOP:
                self._enter_fault(P.FAULT_ESTOP)
                await self._send(P.EVT_STATE, P.pack_evt_state(self.state, self.faults))
            elif t == P.CFG_SET:
                key, value = P.unpack_cfg_set(fr.payload)
                await self._on_cfg_set(key, value)
            elif t == P.CFG_GET:
                key = P.unpack_cfg_get(fr.payload)
                if key in self.cfg_values:
                    await self._send(P.CFG_VAL, P.pack_cfg_val(key, self.cfg_values[key], P.CFG_OK))
                else:
                    await self._send(P.CFG_VAL, P.pack_cfg_val(key, 0.0, P.CFG_UNKNOWN_KEY))
            elif t == P.CFG_SAVE:
                self.saves += 1
            elif t == P.CAL_START:
                if self.state in (P.ST_STANDBY, P.ST_FAULT, P.ST_BOOT):
                    self._cal_end = self.clock() + self.cfg.cal_s
            # unknown types are ignored, like the firmware would
        except (P.ProtocolError, struct.error):
            self.crc_errors += 1

    def _on_drive(self, v_mm: int, w_mrad: int, mode: int, epoch: int) -> None:
        self.drive_frames += 1
        if epoch != 0:
            diff = (epoch - self._max_epoch) & 0xFF
            if self._max_epoch and diff >= 128:  # older than the newest lease seen: stale queue
                self.stale_epoch_drops += 1
                return
            self._max_epoch = epoch
        self._last_drive = self.clock()
        c = self.cfg
        lim_v = min(c.v_limit, self.cfg_values[16] * 1.6)
        lim_w = min(c.w_limit, self.cfg_values[17])
        self._v_target = max(-lim_v, min(lim_v, v_mm / 1000.0))
        self._w_target = max(-lim_w, min(lim_w, w_mrad / 1000.0))

    def _on_arm(self) -> None:
        if (self.state == P.ST_STANDBY and self.faults == 0 and self.vbat >= self.cfg.vbat_low
                and abs(self.pitch_deg) < 5.0):
            self._arm_pending = True
            self._arm_t = 0.0
            self._upright_t = 0.0
        else:
            self.refused += 1

    async def _on_cfg_set(self, key: int, value: float) -> None:
        k = cfgkeys.BY_ID.get(key)
        if k is None:
            await self._send(P.CFG_VAL, P.pack_cfg_val(key, 0.0, P.CFG_UNKNOWN_KEY))
            return
        cur = self.cfg_values[key]
        if k.safety and self._motors_active():
            await self._send(P.CFG_VAL, P.pack_cfg_val(key, cur, P.CFG_REJECTED))
            return
        if not math.isfinite(value) or value < k.lo or value > k.hi:
            await self._send(P.CFG_VAL, P.pack_cfg_val(key, cur, P.CFG_OUT_OF_RANGE))
            return
        self.cfg_values[key] = float(struct.unpack("<f", struct.pack("<f", value))[0])
        await self._send(P.CFG_VAL, P.pack_cfg_val(key, self.cfg_values[key], P.CFG_OK))

    # ---- periodic ---------------------------------------------------------------
    async def _tick_loop(self) -> None:
        while True:
            await asyncio.sleep(self.cfg.tick_s)
            if self._hung:
                self._last_tick = self.clock()
                continue
            await self._tick()

    async def _tick(self) -> None:
        now = self.clock()
        dt = now - self._last_tick
        self._last_tick = now
        c = self.cfg
        link_ok = now - self._last_rx <= c.link_lost_s
        drive_fresh = now - self._last_drive <= c.drive_stale_s
        prev_state = self.state

        # state machine (mirrors core/src/state.c)
        ap = abs(self.pitch_deg)
        if self.state == P.ST_BOOT and now - self._state_t0 >= c.boot_s:
            self._enter(P.ST_CALIBRATING)
        elif self.state == P.ST_CALIBRATING and now - self._state_t0 >= c.cal_s:
            self._enter(P.ST_STANDBY)
        elif self.state == P.ST_STANDBY and self._arm_pending:
            self._arm_t += dt
            self._upright_t = self._upright_t + dt if (ap < 5.0 and self.vbat >= c.vbat_low) else 0.0
            if self._upright_t >= c.arm_hold_s:
                self._enter(P.ST_BALANCING)
                self._link_lost_since = None
            elif self._arm_t >= 10.0:
                self._arm_pending = False
        elif self.state == P.ST_BALANCING:
            if ap > c.tip_deg:
                self._enter(P.ST_FALLEN)
            elif self.vbat < c.vbat_cut:
                self._enter(P.ST_STANDBY)
            else:
                if link_ok:
                    self._link_lost_since = None
                elif self._link_lost_since is None:
                    self._link_lost_since = now
                lost = self._link_lost_since is not None and now - self._link_lost_since >= c.laydown_after_s
                if lost or self.vbat < c.vbat_low:
                    self._enter(P.ST_LAYING_DOWN)
        elif self.state == P.ST_LAYING_DOWN:
            if now - self._state_t0 >= c.laydown_s or ap > 70.0:
                self._enter(P.ST_STANDBY)
        elif self.state == P.ST_FALLEN:
            self._upright_t = self._upright_t + dt if ap < 5.0 else 0.0
            if self._upright_t >= 2.0 and self._disarm_seen:
                self._enter(P.ST_STANDBY)
        if self._cal_end is not None and now >= self._cal_end:
            self._cal_end = None
            self.cal_results += 1
            await self._send(P.CAL_RESULT, b"\x00")

        # motion stub
        if self._motors_active() and link_ok and drive_fresh:
            vt, wt = self._v_target, self._w_target
        else:
            vt = wt = 0.0
        dv = max(-c.a_max * dt, min(c.a_max * dt, vt - self.v))
        self.v += dv
        self._accel = dv / dt if dt > 0 else 0.0
        dw = max(-c.alpha_max * dt, min(c.alpha_max * dt, wt - self.w))
        self.w += dw
        if not self._motors_active():
            self.v = self.w = 0.0
            self._accel = 0.0

        if self.state != prev_state or now - self._last_evt >= c.evt_period_s:
            self._last_evt = now
            await self._send(P.EVT_STATE, P.pack_evt_state(self.state, self.faults))
        if now - self._last_tlm >= c.tlm_period_s:
            self._last_tlm = now
            await self._send(P.TLM_FAST, self._telemetry(now).pack())

    def _telemetry(self, now: float) -> P.TlmFast:
        def clamp16(x: float) -> int:
            return int(max(-32768, min(32767, round(x))))

        return P.TlmFast(
            t_us=int((now - self._t0) * 1e6) & 0xFFFFFFFF,
            pitch_cdeg=clamp16(self.pitch_deg * 100.0),
            pitch_rate_dps10=0,
            yaw_rate_dps10=clamp16(math.degrees(self.w) * 10.0),
            v_mm_s=clamp16(self.v * 1000.0),
            u_l_mv=clamp16((self.v * 10.2) * 1000.0),
            u_r_mv=clamp16((self.v * 10.2) * 1000.0),
            vbat_mv=int(max(0, min(65535, self.vbat * 1000.0))),
            state=self.state,
            faults=self.faults & 0xFF,
        )


async def serve_unix(path: str, cfg: Optional[FakeConfig] = None) -> None:
    """Run a fake M4F behind a SOCK_SEQPACKET unix socket, so balbotd (or a tool) can connect like to /dev/rpmsgN."""
    import os
    import socket

    from .link import FdLink

    if os.path.exists(path):
        os.unlink(path)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    srv.bind(path)
    srv.listen(1)
    srv.setblocking(False)
    loop = asyncio.get_running_loop()
    fake = None
    try:
        while True:
            conn, _ = await loop.sock_accept(srv)
            if fake:
                await fake.stop()
                fake.link.close()
            fd = os.dup(conn.fileno())
            conn.close()
            fake = FakeM4F(FdLink(fd), cfg)
            fake.start()
    finally:
        srv.close()
        if os.path.exists(path):
            os.unlink(path)


def main(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(prog="python -m balbotd.fakem4",
                                 description="Fake M4F behind a unix SOCK_SEQPACKET socket (for balbotd --m4-socket)")
    ap.add_argument("--socket", required=True)
    args = ap.parse_args(argv)
    try:
        asyncio.run(serve_unix(args.socket))
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
