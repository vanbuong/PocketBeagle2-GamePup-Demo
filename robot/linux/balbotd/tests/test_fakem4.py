# SPDX-License-Identifier: GPL-2.0-only
"""Behaviour of the fake M4F on the wire, driven by raw rpmsg frames (no supervisor involved)."""
import asyncio

import pytest

from balbotd import protocol as P
from balbotd.fakem4 import FakeConfig, FakeM4F
from balbotd.link import MemoryLink
from conftest import wait_until


class Wire:
    def __init__(self, cfg: FakeConfig):
        self.host, self.m4 = MemoryLink.pair()
        self.fake = FakeM4F(self.m4, cfg)
        self.seq = 0
        self.frames: list[P.Frame] = []
        self._task = None

    def start(self):
        self.fake.start()
        self._task = asyncio.create_task(self._collect())

    async def _collect(self):
        while True:
            self.frames.append(P.decode(await self.host.recv()))

    async def stop(self):
        self._task.cancel()
        await self.fake.stop()

    async def tx(self, type_, payload=b""):
        self.seq = (self.seq + 1) & 0xFFFF
        await self.host.send(P.encode(type_, self.seq, payload))

    def last(self, type_):
        return next((f for f in reversed(self.frames) if f.type == type_), None)

    def of(self, type_):
        return [f for f in self.frames if f.type == type_]


@pytest.fixture
async def wire():
    w = Wire(FakeConfig(arm_hold_s=0.1, cal_s=0.1, boot_s=0.02, link_lost_s=0.2, laydown_after_s=0.4, laydown_s=0.2,
                        drive_stale_s=0.15))
    w.start()
    yield w
    await w.stop()


async def beat(w: Wire, seconds: float, drive=None, epoch=1):
    """Feed heartbeats (and optionally drive commands) at 50 Hz for `seconds`."""
    end = asyncio.get_running_loop().time() + seconds
    while asyncio.get_running_loop().time() < end:
        await w.tx(P.CMD_HEARTBEAT, P.pack_heartbeat(0))
        if drive is not None:
            await w.tx(P.CMD_DRIVE, P.pack_cmd_drive(drive[0], drive[1], 0, epoch))
        await asyncio.sleep(0.02)


async def arm(w: Wire):
    assert await wait_until(lambda: w.fake.state == P.ST_STANDBY, 2)
    await w.tx(P.CMD_ARM)
    assert await wait_until(lambda: w.fake.state == P.ST_BALANCING, 2)


async def test_boot_sequence_reaches_standby_and_streams_telemetry(wire):
    assert await wait_until(lambda: wire.fake.state == P.ST_STANDBY, 2)
    await asyncio.sleep(0.1)
    states = [P.unpack_evt_state(f.payload)[0] for f in wire.of(P.EVT_STATE)]
    assert states[:3] == [P.ST_CALIBRATING, P.ST_STANDBY, P.ST_STANDBY][: len(states[:3])] or P.ST_STANDBY in states
    t = P.TlmFast.unpack(wire.last(P.TLM_FAST).payload)
    assert t.state == P.ST_STANDBY and t.vbat_mv == 11100 and t.v_mm_s == 0
    assert len(wire.of(P.TLM_FAST)) >= 5  # ~100 Hz
    seqs = [f.seq for f in wire.frames]
    assert all(((b - a) & 0xFFFF) == 1 for a, b in zip(seqs, seqs[1:])), "tx sequence must be gap free"


async def test_arm_needs_standby_and_upright(wire):
    await wire.tx(P.CMD_ARM)  # still booting: refused
    await asyncio.sleep(0.05)
    assert wire.fake.refused == 1 and wire.fake.state != P.ST_BALANCING
    assert await wait_until(lambda: wire.fake.state == P.ST_STANDBY, 2)
    wire.fake.rest_pitch_deg = 12.0
    await wire.tx(P.CMD_ARM)
    await asyncio.sleep(0.05)
    assert wire.fake.refused == 2
    wire.fake.rest_pitch_deg = 0.0
    await arm(wire)


async def test_drive_ramps_with_the_acceleration_limit_and_is_clamped(wire):
    await arm(wire)
    await beat(wire, 0.5, drive=(300, 0))
    v = P.TlmFast.unpack(wire.last(P.TLM_FAST).payload).v_mm_s
    assert 0 < v <= 300  # 0.8 m/s^2 for ~0.5 s: at most 0.4 m/s, command is 0.3
    await beat(wire, 1.0, drive=(5000, 0))  # request 5 m/s: firmware limit is 0.96 m/s
    assert max(P.TlmFast.unpack(f.payload).v_mm_s for f in wire.of(P.TLM_FAST)) <= 960


async def test_drive_is_ignored_unless_balancing(wire):
    assert await wait_until(lambda: wire.fake.state == P.ST_STANDBY, 2)
    await beat(wire, 0.3, drive=(400, 0))
    assert wire.fake.v == 0.0


async def test_stale_drive_stops_the_robot_even_if_heartbeats_continue(wire):
    await arm(wire)
    await beat(wire, 0.6, drive=(400, 0))
    assert wire.fake.v > 0.1
    await beat(wire, 1.5)  # heartbeats only: drive timeout 0.15 s, then deceleration 0.8 m/s^2
    assert wire.fake.v == pytest.approx(0.0, abs=1e-6)
    assert wire.fake.state == P.ST_BALANCING  # still balancing: it just stands still


async def test_older_lease_epoch_is_dropped(wire):
    await arm(wire)
    await wire.tx(P.CMD_DRIVE, P.pack_cmd_drive(100, 0, 0, 5))
    await wire.tx(P.CMD_DRIVE, P.pack_cmd_drive(100, 0, 0, 4))  # an older lease: stale queue
    await wire.tx(P.CMD_DRIVE, P.pack_cmd_drive(100, 0, 0, 5))
    await asyncio.sleep(0.05)
    assert wire.fake.stale_epoch_drops == 1


async def test_lease_epoch_comparison_is_wrap_safe(wire):
    await arm(wire)
    for e in (250, 255, 3, 4):  # 255 -> 3 wraps through 0 (0 is never a lease epoch)
        await wire.tx(P.CMD_DRIVE, P.pack_cmd_drive(100, 0, 0, e))
    await asyncio.sleep(0.05)
    assert wire.fake.stale_epoch_drops == 0
    await wire.tx(P.CMD_DRIVE, P.pack_cmd_drive(100, 0, 0, 255))  # now older than 4
    await asyncio.sleep(0.05)
    assert wire.fake.stale_epoch_drops == 1


async def test_link_loss_ramps_to_zero_then_lays_down(wire):
    await arm(wire)
    await beat(wire, 0.5, drive=(300, 0))
    assert wire.fake.v > 0.1
    # balbotd goes silent
    assert await wait_until(lambda: wire.fake.v == 0.0, 2)
    assert wire.fake.state == P.ST_BALANCING
    assert await wait_until(lambda: wire.fake.state == P.ST_LAYING_DOWN, 2)
    assert await wait_until(lambda: wire.fake.state == P.ST_STANDBY, 2)


async def test_estop_latches_fault_from_any_state_and_blocks_arm(wire):
    await arm(wire)
    await wire.tx(P.CMD_ESTOP)
    assert await wait_until(lambda: wire.fake.state == P.ST_FAULT, 1)
    assert wire.fake.faults & P.FAULT_ESTOP
    await wire.tx(P.CMD_ARM)
    await asyncio.sleep(0.05)
    assert wire.fake.state == P.ST_FAULT
    ev = P.unpack_evt_state(wire.last(P.EVT_STATE).payload)
    assert ev == (P.ST_FAULT, P.FAULT_ESTOP)


async def test_tip_over_and_recovery(wire):
    await arm(wire)
    wire.fake.tip_over(40)
    assert await wait_until(lambda: wire.fake.state == P.ST_FALLEN, 1)
    assert wire.fake.v == 0.0
    wire.fake.right_up()
    await beat(wire, 0.3)
    assert wire.fake.state == P.ST_FALLEN  # no DISARM yet
    await wire.tx(P.CMD_DISARM)
    assert await wait_until(lambda: wire.fake.state == P.ST_STANDBY, 3)


async def test_low_battery_lays_down(wire):
    await arm(wire)
    wire.fake.set_vbat(9.8)
    assert await wait_until(lambda: wire.fake.state == P.ST_LAYING_DOWN, 1)


async def test_config_get_set_and_rules(wire):
    await wire.tx(P.CFG_GET, P.pack_cfg_get(1))
    assert await wait_until(lambda: wire.last(P.CFG_VAL), 1)
    assert P.unpack_cfg_val(wire.last(P.CFG_VAL).payload) == (1, pytest.approx(60.0), P.CFG_OK)
    await wire.tx(P.CFG_SET, P.pack_cfg_set(1, 42.5))
    await wire.tx(P.CFG_SET, P.pack_cfg_set(1, 9999.0))     # out of range
    await wire.tx(P.CFG_SET, P.pack_cfg_set(1, float("nan")))
    await wire.tx(P.CFG_SET, P.pack_cfg_set(999, 1.0))      # unknown
    assert await wait_until(lambda: len(wire.of(P.CFG_VAL)) >= 5, 1)
    vals = [P.unpack_cfg_val(f.payload) for f in wire.of(P.CFG_VAL)]
    assert [v[2] for v in vals] == [P.CFG_OK, P.CFG_OK, P.CFG_OUT_OF_RANGE, P.CFG_OUT_OF_RANGE, P.CFG_UNKNOWN_KEY]
    assert vals[1][1] == pytest.approx(42.5)
    assert wire.fake.cfg_values[1] == pytest.approx(42.5)  # rejected writes did not change it
    await arm(wire)
    await wire.tx(P.CFG_SET, P.pack_cfg_set(16, 0.3))       # safety key while balancing
    assert await wait_until(lambda: len(wire.of(P.CFG_VAL)) >= 6, 1)
    assert P.unpack_cfg_val(wire.last(P.CFG_VAL).payload)[2] == P.CFG_REJECTED
    await wire.tx(P.CFG_SET, P.pack_cfg_set(1, 50.0))       # non-safety key is fine while balancing
    assert await wait_until(lambda: len(wire.of(P.CFG_VAL)) >= 7, 1)
    assert P.unpack_cfg_val(wire.last(P.CFG_VAL).payload)[2] == P.CFG_OK


async def test_corrupt_and_unknown_frames_do_not_upset_it(wire):
    await wire.host.send(b"\x00\x01garbage")
    await wire.host.send(P.encode(0x7E, 1, b"?"))  # unknown type: ignored
    await wire.host.send(P.encode(P.CMD_DRIVE, 2, b"123"))  # short payload
    await asyncio.sleep(0.05)
    assert wire.fake.crc_errors >= 2
    assert await wait_until(lambda: wire.fake.state == P.ST_STANDBY, 2)


async def test_hang_stops_all_traffic_and_resume_recovers(wire):
    assert await wait_until(lambda: wire.fake.state == P.ST_STANDBY, 2)
    wire.fake.hang()
    await asyncio.sleep(0.05)
    n = len(wire.frames)
    await asyncio.sleep(0.2)
    assert len(wire.frames) == n
    wire.fake.resume()
    assert await wait_until(lambda: len(wire.frames) > n, 1)


async def test_calibration_request_gets_a_result(wire):
    assert await wait_until(lambda: wire.fake.state == P.ST_STANDBY, 2)
    await wire.tx(P.CAL_START, b"\x00")
    assert await wait_until(lambda: wire.last(P.CAL_RESULT) is not None, 1)
