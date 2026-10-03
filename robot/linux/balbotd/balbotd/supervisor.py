# SPDX-License-Identifier: GPL-2.0-only
"""The supervisor: client messages in, rpmsg frames to the M4F out, telemetry back (doc 01, 1.7 and doc 03).

Framework independent: the FastAPI layer (app.py) only authenticates and shuttles text. Everything with a
deadline (drive failsafe, lease expiry, M4F liveness) lives here and takes an injectable clock.
"""
from __future__ import annotations

import asyncio
import collections
import json
import math
import time
from dataclasses import dataclass, field
from typing import Awaitable, Callable, Optional

from . import cfgkeys
from . import protocol as P
from .arbiter import Arbiter, Role, Transport
from .config import Config
from .link import Link, LinkClosed
from .shaping import shape

FAULT_NAMES = {P.FAULT_IMU: "IMU", P.FAULT_OVERRUN: "OVERRUN", P.FAULT_ESTOP: "ESTOP", P.FAULT_ENCODER: "ENCODER",
               P.FAULT_STALL: "STALL", P.FAULT_PRU: "PRU", P.FAULT_CAL: "CAL"}
CFG_STATUS = {P.CFG_OK: "ok", P.CFG_REJECTED: "rejected", P.CFG_UNKNOWN_KEY: "unknown_key",
              P.CFG_OUT_OF_RANGE: "out_of_range"}
CAL_KINDS = {"gyro": 0, "level": 1}


class ClientViolation(Exception):
    """The client broke a protocol limit (size, rate, queue): the caller must disconnect it."""


class Client:
    def __init__(self, cid: str, name: str, role: Role, transport: Transport, queue_size: int, rate: int,
                 clock: Callable[[], float]):
        self.id = cid
        self.name = name
        self.role = role
        self.transport = transport
        self.out: asyncio.Queue = asyncio.Queue(maxsize=queue_size)
        self.sub_rate = 10.0
        self.last_tlm = 0.0
        self.seq = P.SeqTracker()
        self.offsets: collections.deque = collections.deque(maxlen=64)
        self.intent = (0.0, 0.0, False)
        self.intent_t = -math.inf
        self.closed = False
        self._rate = rate
        self._tokens = float(rate)
        self._clock = clock
        self._t = clock()

    def allow(self) -> bool:
        now = self._clock()
        self._tokens = min(float(self._rate), self._tokens + (now - self._t) * self._rate)
        self._t = now
        if self._tokens < 1.0:
            return False
        self._tokens -= 1.0
        return True

    def send(self, msg: dict) -> None:
        if self.closed:
            return
        try:
            self.out.put_nowait(msg)
        except asyncio.QueueFull as e:
            self.closed = True
            raise ClientViolation("outgoing queue full (slow client)") from e

    def note_ts(self, ts_ms: float, srv_now: float) -> float:
        """Record a client timestamp, return the extra delay (s) relative to the fastest message seen recently."""
        self.offsets.append(srv_now - ts_ms / 1000.0)
        return (srv_now - ts_ms / 1000.0) - min(self.offsets)


@dataclass
class Stats:
    frames_rx: int = 0
    frames_tx: int = 0
    crc_errors: int = 0
    seq_gaps: int = 0
    seq_dups: int = 0
    drive_forwarded: int = 0
    drive_dropped_stale: int = 0
    drive_dropped_seq: int = 0
    link_send_dropped: int = 0
    reconnects: int = 0
    violations: int = 0
    clients_total: int = 0


class Supervisor:
    def __init__(self, link: Optional[Link], cfg: Config, clock: Callable[[], float] = time.monotonic,
                 link_factory: Optional[Callable[[], Awaitable[Link]]] = None):
        self.link = link
        self.link_factory = link_factory
        self.cfg = cfg
        self.clock = clock
        self.arbiter = Arbiter(clock, cfg.timing.lease_timeout_s)
        self.clients: dict[str, Client] = {}
        self.stats = Stats()
        self.state = P.ST_BOOT
        self.faults = 0
        self.last_tlm: Optional[P.TlmFast] = None
        self.m4f_alive = False
        self.cfg_cache: dict[str, float] = {k.name: k.default for k in cfgkeys.KEYS}
        self._tx_seq = 0
        self._rx_seq = P.SeqTracker()
        self._t0 = clock()
        self._last_m4f_rx = -math.inf
        self._pending_cfg: dict[int, asyncio.Future] = {}
        self._tasks: list[asyncio.Task] = []
        self._next_id = 0
        self._last_state_pub: Optional[tuple] = None
        self._bg: set[asyncio.Task] = set()
        self._last_critical: dict[int, float] = {}

    # ---- clients ---------------------------------------------------------------------
    def add_client(self, name: str, role: Role, transport: Transport = Transport.WIFI) -> Client:
        self._next_id += 1
        c = Client(f"c{self._next_id}", name, role, transport, self.cfg.limits.queue_size,
                   self.cfg.limits.max_msgs_per_s, self.clock)
        self.clients[c.id] = c
        self.stats.clients_total += 1
        c.send({"t": "hello", "proto": P.VERSION, "client": c.id, "role": role.name.lower(),
                "epoch": self.arbiter.epoch, "m4f_alive": self.m4f_alive})
        c.send(self.state_msg())
        return c

    def remove_client(self, c: Client) -> None:
        c.closed = True
        self.clients.pop(c.id, None)
        if self.arbiter.drop_client(c.id):
            self._publish_state(force=True)

    def _broadcast(self, msg: dict) -> None:
        for c in list(self.clients.values()):
            try:
                c.send(msg)
            except ClientViolation:
                self.stats.violations += 1

    # ---- messages from clients ----------------------------------------------------------
    async def handle(self, c: Client, text: str) -> None:
        """Process one client message. Raises ClientViolation if the client must be disconnected."""
        if len(text.encode("utf-8", "ignore")) > self.cfg.limits.max_message_bytes:
            self.stats.violations += 1
            raise ClientViolation("message too large")
        if not c.allow():
            self.stats.violations += 1
            raise ClientViolation("rate limit exceeded")
        try:
            msg = json.loads(text)
        except (ValueError, RecursionError):
            return self._err(c, "BAD_JSON", "not valid JSON")
        if not isinstance(msg, dict) or not isinstance(msg.get("t"), str):
            return self._err(c, "BAD_MESSAGE", "expected an object with a string field t")
        fn = getattr(self, "_on_" + msg["t"], None) if msg["t"].isidentifier() else None
        if fn is None or msg["t"].startswith("_"):
            return self._err(c, "UNKNOWN_TYPE", f"unknown message type {msg['t'][:32]!r}")
        await fn(c, msg)

    @staticmethod
    def _err(c: Client, code: str, text: str) -> None:
        c.send({"t": "err", "code": code, "msg": text})

    @staticmethod
    def _num(msg: dict, key: str, lo: float = -math.inf, hi: float = math.inf) -> Optional[float]:
        v = msg.get(key)
        if isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v) or v < lo or v > hi:
            return None
        return float(v)

    async def _on_ping(self, c: Client, msg: dict) -> None:
        ts = self._num(msg, "ts")
        now = self.clock()
        if ts is not None:
            c.note_ts(ts, now)
        if self.arbiter.touch(c.id):
            pass
        c.send({"t": "pong", "ts": ts, "srv": int(now * 1000)})

    async def _on_sub(self, c: Client, msg: dict) -> None:
        r = self._num(msg, "rate", 0, 1000)
        if r is None:
            return self._err(c, "BAD_ARG", "rate must be a number")
        c.sub_rate = min(r, 50.0)

    async def _on_lease(self, c: Client, msg: dict) -> None:
        action = msg.get("action")
        if action == "request":
            ok, epoch, reason = self.arbiter.request(c.id, c.role, c.transport)
            if ok and reason != "already":
                c.intent_t = -math.inf  # a new lease never starts from a command sent under an older one
            c.send({"t": "lease", "granted": ok, "epoch": epoch, "reason": reason})
            if ok and reason != "already":
                self._publish_state(force=True)
        elif action == "release":
            was = self.arbiter.release(c.id)
            c.send({"t": "lease", "granted": False, "epoch": 0, "reason": "released" if was else "not_holder"})
            if was:
                self._publish_state(force=True)
        else:
            self._err(c, "BAD_ARG", "action must be request or release")

    async def _on_drive(self, c: Client, msg: dict) -> None:
        if not self.arbiter.is_driver(c.id):
            return self._err(c, "NOT_DRIVER", "request the lease first")
        seq, vx, wz = self._num(msg, "seq", 0, 65535), self._num(msg, "vx", -1e3, 1e3), self._num(msg, "wz", -1e3, 1e3)
        flags = self._num(msg, "flags", 0, 255)
        if seq is None or vx is None or wz is None or (msg.get("flags") is not None and flags is None):
            return self._err(c, "BAD_ARG", "drive needs numeric seq, vx, wz")
        if c.seq.check(int(seq)) == "dup":
            self.stats.drive_dropped_seq += 1
            return
        ts = self._num(msg, "ts")
        now = self.clock()
        if ts is not None and c.note_ts(ts, now) > self.cfg.timing.max_drive_age_s:
            self.stats.drive_dropped_stale += 1
            return
        self.arbiter.touch(c.id)
        c.intent = (vx, wz, bool(int(flags or 0) & 1 << 1))
        c.intent_t = now

    async def _on_arm(self, c: Client, msg: dict) -> None:
        if not (c.role == Role.ADMIN or self.arbiter.is_driver(c.id)):
            return self._err(c, "NOT_DRIVER", "arming needs the lease")
        await self._send(P.CMD_ARM)

    async def _on_reset(self, c: Client, msg: dict) -> None:
        if not (c.role == Role.ADMIN or self.arbiter.is_driver(c.id)):
            return self._err(c, "NOT_DRIVER", "reset needs the lease")
        await self._send(P.CMD_RESET)  # not repeated: a late duplicate could clear a fresh fault

    async def _on_disarm(self, c: Client, msg: dict) -> None:
        await self._send_critical(P.CMD_DISARM)

    async def _on_estop(self, c: Client, msg: dict) -> None:
        # accepted from ANY authenticated client, in any role
        await self._send_critical(P.CMD_ESTOP)

    async def _on_cal(self, c: Client, msg: dict) -> None:
        if c.role != Role.ADMIN:
            return self._err(c, "FORBIDDEN", "admin only")
        kind = CAL_KINDS.get(msg.get("kind"))
        if kind is None:
            return self._err(c, "BAD_ARG", f"kind must be one of {sorted(CAL_KINDS)}")
        await self._send(P.CAL_START, bytes([kind]))

    async def _on_cfg_get(self, c: Client, msg: dict) -> None:
        k = cfgkeys.BY_NAME.get(msg.get("key"))
        if k is None:
            return self._err(c, "UNKNOWN_KEY", "unknown configuration key")
        await self._cfg_roundtrip(c, k, P.CFG_GET, P.pack_cfg_get(k.id))

    async def _on_cfg_set(self, c: Client, msg: dict) -> None:
        if c.role != Role.ADMIN:
            return self._err(c, "FORBIDDEN", "admin only")
        k = cfgkeys.BY_NAME.get(msg.get("key"))
        if k is None:
            return self._err(c, "UNKNOWN_KEY", "unknown configuration key")
        v = self._num(msg, "value")
        if v is None or v < k.lo or v > k.hi:
            return self._err(c, "OUT_OF_RANGE", f"{k.name} must be within [{k.lo}, {k.hi}]")
        if k.safety and self.state in (P.ST_BALANCING, P.ST_LAYING_DOWN):
            return self._err(c, "UNSAFE_WHILE_BALANCING", f"{k.name} can only change while disarmed")
        await self._cfg_roundtrip(c, k, P.CFG_SET, P.pack_cfg_set(k.id, v))

    async def _on_cfg_save(self, c: Client, msg: dict) -> None:
        if c.role != Role.ADMIN:
            return self._err(c, "FORBIDDEN", "admin only")
        await self._send(P.CFG_SAVE)

    async def _cfg_roundtrip(self, c: Client, k: cfgkeys.Key, type_: int, payload: bytes) -> None:
        if k.id in self._pending_cfg:
            return self._err(c, "BUSY", "another request for this key is in flight")
        fut = asyncio.get_running_loop().create_future()
        self._pending_cfg[k.id] = fut
        try:
            await self._send(type_, payload)
            try:
                value, status = await asyncio.wait_for(fut, self.cfg.timing.cfg_reply_timeout_s)
            except asyncio.TimeoutError:
                return self._err(c, "TIMEOUT", "no reply from the M4F")
            c.send({"t": "cfg", "key": k.name, "value": round(value, 6), "status": CFG_STATUS.get(status, "error")})
        finally:
            self._pending_cfg.pop(k.id, None)

    # ---- link to the M4F -------------------------------------------------------------------
    async def _send(self, type_: int, payload: bytes = b"") -> bool:
        if self.link is None or self.link.closed:
            return False
        self._tx_seq = (self._tx_seq + 1) & 0xFFFF
        try:
            ok = await self.link.send(P.encode(type_, self._tx_seq, payload))
        except LinkClosed:
            return False
        if ok:
            self.stats.frames_tx += 1
        else:
            self.stats.link_send_dropped += 1
        return ok

    async def _send_critical(self, type_: int) -> None:
        """DISARM / ESTOP: send now, then twice more shortly after (idempotent, survives a dropped frame).

        Repeats of the same command within 40 ms (the page itself sends E-STOP three times) are coalesced: the resends
        already scheduled cover them, so the M4F sees three frames, not nine.
        """
        now = self.clock()
        if now - self._last_critical.get(type_, -math.inf) < 0.040:
            return
        self._last_critical[type_] = now
        await self._send(type_)
        t = asyncio.get_running_loop().create_task(self._resend(type_))
        self._bg.add(t)
        t.add_done_callback(self._bg.discard)

    async def _resend(self, type_: int) -> None:
        for _ in range(2):
            await asyncio.sleep(0.015)
            await self._send(type_)

    def _on_frame(self, data: bytes) -> None:
        try:
            fr = P.decode(data)
        except P.ProtocolError:
            self.stats.crc_errors += 1
            return
        r = self._rx_seq.check(fr.seq)
        self.stats.seq_gaps = self._rx_seq.gaps
        self.stats.seq_dups = self._rx_seq.dups
        if r == "dup":
            return
        self.stats.frames_rx += 1
        self._last_m4f_rx = self.clock()
        if not self.m4f_alive:
            self.m4f_alive = True
            self._publish_state(force=True)
        try:
            if fr.type == P.TLM_FAST:
                self._on_tlm(P.TlmFast.unpack(fr.payload))
            elif fr.type == P.EVT_STATE:
                st, faults = P.unpack_evt_state(fr.payload)
                self._set_state(st, faults)
            elif fr.type == P.CFG_VAL:
                key, value, status = P.unpack_cfg_val(fr.payload)
                k = cfgkeys.BY_ID.get(key)
                if status == P.CFG_OK and k:
                    self.cfg_cache[k.name] = value
                fut = self._pending_cfg.get(key)
                if fut and not fut.done():
                    fut.set_result((value, status))
        except P.ProtocolError:
            self.stats.crc_errors += 1

    def _set_state(self, st: int, faults: int) -> None:
        self.state, self.faults = st, faults
        self._publish_state()

    def _on_tlm(self, t: P.TlmFast) -> None:
        self.last_tlm = t
        if t.state != self.state or t.faults != (self.faults & 0xFF):
            self._set_state(t.state, t.faults)
        now = self.clock()
        msg = None
        for c in list(self.clients.values()):
            if c.sub_rate > 0 and now - c.last_tlm >= 1.0 / c.sub_rate - 1e-9:
                c.last_tlm = now
                msg = msg or {"t": "tlm", "ts": t.t_us // 1000, "pitch": t.pitch_cdeg / 100.0,
                              "pitch_rate": t.pitch_rate_dps10 / 10.0, "yaw_rate": t.yaw_rate_dps10 / 10.0,
                              "v": t.v_mm_s / 1000.0, "uL": t.u_l_mv / 1000.0, "uR": t.u_r_mv / 1000.0,
                              "vbat": t.vbat_mv / 1000.0, "state": P.STATES[t.state] if t.state < len(P.STATES) else "?"}
                try:
                    c.send(msg)
                except ClientViolation:
                    self.stats.violations += 1

    def state_msg(self) -> dict:
        lease = self.arbiter.lease
        holder = self.clients[lease.holder].name if lease and lease.holder in self.clients else None
        names = [n for b, n in FAULT_NAMES.items() if self.faults & b]
        if not self.m4f_alive:
            names.append("M4F_DEAD")
        return {"t": "state", "state": P.STATES[self.state] if self.state < len(P.STATES) else "?",
                "faults": self.faults, "fault_names": names, "m4f_alive": self.m4f_alive,
                "driver": holder, "epoch": self.arbiter.epoch,
                "vbat": (self.last_tlm.vbat_mv / 1000.0) if self.last_tlm else None}

    def _publish_state(self, force: bool = False) -> None:
        msg = self.state_msg()
        key = (msg["state"], msg["faults"], msg["m4f_alive"], msg["driver"], msg["epoch"])
        if force or key != self._last_state_pub:
            self._last_state_pub = key
            self._broadcast(msg)

    # ---- periodic work -------------------------------------------------------------------------
    def current_drive(self) -> tuple[int, int, int]:
        """(v_mm_s, w_mrad_s, epoch) to send now: the lease holder's fresh intent, otherwise zero."""
        lease = self.arbiter.lease
        if lease is not None:
            c = self.clients.get(lease.holder)
            if c and self.clock() - c.intent_t <= self.cfg.timing.drive_failsafe_s:
                v, w = shape(c.intent[0], c.intent[1], self.cfg.shaping, boost=c.intent[2])
                return v, w, lease.epoch
            return 0, 0, lease.epoch
        return 0, 0, 0

    async def _drive_loop(self) -> None:
        while True:
            v, w, epoch = self.current_drive()
            if await self._send(P.CMD_DRIVE, P.pack_cmd_drive(v, w, 0, epoch)):
                self.stats.drive_forwarded += 1
            await asyncio.sleep(self.cfg.timing.drive_period_s)

    async def _heartbeat_loop(self) -> None:
        while True:
            await self._send(P.CMD_HEARTBEAT, P.pack_heartbeat(int((self.clock() - self._t0) * 1000)))
            await asyncio.sleep(self.cfg.timing.heartbeat_period_s)

    async def _watch_loop(self) -> None:
        while True:
            await asyncio.sleep(0.02)
            old = self.arbiter.expire()
            if old is not None:
                c = self.clients.get(old)
                if c:
                    try:
                        c.send({"t": "lease", "granted": False, "epoch": 0, "reason": "expired"})
                    except ClientViolation:
                        self.stats.violations += 1
                self._publish_state(force=True)
            if self.m4f_alive and self.clock() - self._last_m4f_rx > self.cfg.timing.m4f_dead_s:
                self.m4f_alive = False
                self._publish_state(force=True)

    async def _link_loop(self) -> None:
        backoff = 0.2
        while True:
            if self.link is None or self.link.closed:
                if self.link_factory is None:
                    return
                try:
                    self.link = await self.link_factory()
                    self.stats.reconnects += 1
                    backoff = 0.2
                except Exception:
                    await asyncio.sleep(backoff)
                    backoff = min(backoff * 2, 5.0)
                    continue
            try:
                data = await self.link.recv()
            except LinkClosed:
                self.m4f_alive = False
                self._publish_state(force=True)
                if self.link_factory is None:
                    return
                await asyncio.sleep(backoff)
                continue
            self._on_frame(data)

    def start(self) -> None:
        loop = asyncio.get_running_loop()
        self._tasks = [loop.create_task(f()) for f in
                       (self._link_loop, self._drive_loop, self._heartbeat_loop, self._watch_loop)]

    async def stop(self) -> None:
        for t in self._tasks + list(self._bg):
            t.cancel()
        await asyncio.gather(*self._tasks, *self._bg, return_exceptions=True)
        self._tasks = []
        if self.link:
            self.link.close()
