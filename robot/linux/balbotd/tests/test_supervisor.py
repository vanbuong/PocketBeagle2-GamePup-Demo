# SPDX-License-Identifier: GPL-2.0-only
"""Supervisor against the fake M4F over an in-memory link: the behaviours promised in docs 01 and 03."""
import asyncio
import json
import math
import time

import pytest

from balbotd import protocol as P
from balbotd.arbiter import Role, Transport
from balbotd.config import Config, Limits
from balbotd.fakem4 import FakeConfig
from balbotd.link import MemoryLink
from balbotd.supervisor import ClientViolation, Supervisor
from conftest import Rig, wait_until


async def test_hello_and_state_on_connect(rig):
    c = rig.client("alice", Role.VIEWER)
    msgs = c.drain()
    assert msgs[0]["t"] == "hello" and msgs[0]["proto"] == 1 and msgs[0]["role"] == "viewer"
    assert msgs[1]["t"] == "state" and msgs[1]["driver"] is None


async def test_lease_rules_and_state_broadcast(rig):
    a, b, v = rig.client("a"), rig.client("b"), rig.client("v", Role.VIEWER)
    for x in (a, b, v):
        x.drain()
    g = await a.lease()
    assert g == {"t": "lease", "granted": True, "epoch": 1, "reason": "granted"}
    assert (await b.lease())["reason"] == "busy"
    await v.send({"t": "lease", "action": "request"})
    assert (await v.recv("lease"))["reason"] == "role"
    st = await v.recv("state", driver="a")  # everyone is told who drives
    assert st["epoch"] == 1
    await a.send({"t": "lease", "action": "release"})
    assert (await a.recv("lease"))["reason"] == "released"
    assert (await b.lease())["epoch"] == 2


async def test_drive_requires_the_lease(rig):
    b = rig.client("b")
    b.drain()
    await b.drive(0.5)
    err = await b.recv("err")
    assert err["code"] == "NOT_DRIVER"


async def test_drive_reaches_the_robot_shaped_and_clamped(rig):
    c = rig.client()
    await rig.to_balancing(c)
    for _ in range(40):  # 0.8 s of full stick
        await c.drive(1.0, 0.0)
        await asyncio.sleep(0.02)
    assert rig.fake.v > 0.3
    assert rig.fake._v_target == pytest.approx(0.6)  # v_max at full stick, after the expo curve
    await c.drive(1.0, 0.0, flags=2)  # boost: 0.96 m/s, equal to the firmware limit
    await asyncio.sleep(0.1)
    assert rig.fake._v_target == pytest.approx(0.96)


async def test_deadband_means_zero_command(rig):
    c = rig.client()
    await rig.to_balancing(c)
    await c.drive(0.04, -0.04)
    await asyncio.sleep(0.1)
    assert rig.fake._v_target == 0.0 and rig.fake._w_target == 0.0


async def test_failsafe_zeroes_the_command_within_250_ms_and_keeps_balancing(rig):
    c = rig.client()
    await rig.to_balancing(c)
    for _ in range(30):
        await c.drive(0.8)
        await asyncio.sleep(0.02)
    assert rig.fake._v_target > 0.2
    t_stop = time.monotonic()  # the client stops talking (app crashed / WiFi dropped)
    assert await wait_until(lambda: rig.fake._v_target == 0.0, 1.0, 0.005)
    dt = time.monotonic() - t_stop
    assert 0.2 <= dt <= 0.4, f"failsafe took {dt * 1000:.0f} ms"  # 250 ms + one 20 ms send period + slack
    assert rig.fake.state == P.ST_BALANCING
    assert await wait_until(lambda: rig.fake.v == 0.0, 2.0)


async def test_lease_expires_and_a_late_drive_is_refused(rig):
    c = rig.client()
    await rig.to_balancing(c)
    c.drain()
    await c.recv("lease", timeout=2.0, reason="expired")
    assert rig.sup.arbiter.lease is None
    await c.drive(0.5)
    assert (await c.recv("err"))["code"] == "NOT_DRIVER"


async def test_ping_keeps_the_lease_alive(rig):
    c = rig.client()
    assert (await c.lease())["granted"]
    for _ in range(25):  # 1.25 s > lease timeout of 1 s, but pings arrive every 50 ms
        await c.send({"t": "ping", "ts": 1})
        await asyncio.sleep(0.05)
    assert rig.sup.arbiter.is_driver(c.c.id)


async def test_new_lease_never_inherits_the_previous_drivers_command(rig):
    a, b = rig.client("a"), rig.client("b")
    await rig.to_balancing(a)
    await a.drive(1.0)
    await asyncio.sleep(0.1)
    assert rig.fake._v_target > 0
    await a.send({"t": "lease", "action": "release"})
    assert (await b.lease())["granted"]
    await asyncio.sleep(0.15)
    assert rig.fake._v_target == 0.0  # b has not sent anything yet
    assert rig.fake.stale_epoch_drops == 0


async def test_releasing_and_re_requesting_does_not_resurrect_the_old_command(rig):
    a = rig.client("a")
    await rig.to_balancing(a)
    await a.drive(1.0)
    assert rig.sup.current_drive()[0] == 600
    await a.send({"t": "lease", "action": "release"})
    await a.recv("lease", reason="released")
    g = await a.lease()  # immediately, well inside the 250 ms freshness window of the old command
    assert g["granted"] and g["reason"] == "granted"
    assert rig.sup.current_drive() == (0, 0, g["epoch"])
    await a.drive(0.5)
    assert rig.sup.current_drive()[0] > 0


async def test_transport_preemption(rig):
    ble = rig.client("ble", transport=Transport.BLE)
    wifi = rig.client("wifi", transport=Transport.WIFI)
    assert (await ble.lease())["granted"]
    g = await wifi.lease()
    assert g["granted"] and g["reason"] == "preempted" and g["epoch"] == 2
    await ble.drive(1.0)
    assert (await ble.recv("err"))["code"] == "NOT_DRIVER"


async def test_estop_works_from_a_viewer_and_survives_a_dropped_frame(rig):
    v = rig.client("v", Role.VIEWER)
    await rig.to_balancing(rig.client())
    dropped = []

    def drop_first_estop(data):
        fr = P.decode(data)
        if fr.type == P.CMD_ESTOP and not dropped:
            dropped.append(1)
            return True
        return False

    rig.a.drop_filter = drop_first_estop
    await v.send({"t": "estop"})
    assert await wait_until(lambda: rig.fake.state == P.ST_FAULT, 1.0)
    assert dropped and rig.fake.faults & P.FAULT_ESTOP


async def test_disarm_from_any_client(rig):
    d = rig.client()
    v = rig.client("v", Role.VIEWER)
    await rig.to_balancing(d)
    await v.send({"t": "disarm"})
    assert await wait_until(lambda: rig.fake.state == P.ST_STANDBY, 1.0)


async def test_arm_permissions(rig):
    assert await wait_until(lambda: rig.fake.state == P.ST_STANDBY, 3)
    v, d, adm = rig.client("v", Role.VIEWER), rig.client("d"), rig.client("adm", Role.ADMIN)
    for x in (v, d):
        x.drain()
        await x.send({"t": "arm"})
        assert (await x.recv("err"))["code"] == "NOT_DRIVER"  # viewer, and a driver without the lease
    await adm.send({"t": "arm"})
    assert await wait_until(lambda: rig.fake.state == P.ST_BALANCING, 2.0)


# ---- configuration ---------------------------------------------------------------------------------
async def test_cfg_set_round_trip_updates_the_cache(rig):
    adm = rig.client("adm", Role.ADMIN)
    adm.drain()
    await adm.send({"t": "cfg_set", "key": "pid.angle.kp", "value": 42.5})
    r = await adm.recv("cfg")
    assert r == {"t": "cfg", "key": "pid.angle.kp", "value": 42.5, "status": "ok"}
    assert rig.sup.cfg_cache["pid.angle.kp"] == 42.5 and rig.fake.cfg_values[1] == 42.5
    await adm.send({"t": "cfg_get", "key": "pid.angle.kd"})
    assert (await adm.recv("cfg"))["value"] == pytest.approx(7.5)


async def test_cfg_validation_and_permissions(rig):
    adm, drv = rig.client("adm", Role.ADMIN), rig.client("drv")
    adm.drain()
    drv.drain()
    await drv.send({"t": "cfg_set", "key": "pid.angle.kp", "value": 1})
    assert (await drv.recv("err"))["code"] == "FORBIDDEN"
    for msg, code in (({"t": "cfg_set", "key": "nope", "value": 1}, "UNKNOWN_KEY"),
                      ({"t": "cfg_set", "key": "pid.angle.kp", "value": 1e9}, "OUT_OF_RANGE"),
                      ({"t": "cfg_set", "key": "pid.angle.kp", "value": "7"}, "OUT_OF_RANGE"),
                      ({"t": "cfg_set", "key": "pid.angle.kp"}, "OUT_OF_RANGE")):
        await adm.send(msg)
        assert (await adm.recv("err"))["code"] == code
    await adm.send_raw('{"t":"cfg_set","key":"pid.angle.kp","value":NaN}')
    assert (await adm.recv("err"))["code"] == "OUT_OF_RANGE"
    assert rig.fake.cfg_values[1] == 60.0  # nothing reached the robot


async def test_safety_keys_refused_while_balancing_but_allowed_when_disarmed(rig):
    adm = rig.client("adm", Role.ADMIN)
    await rig.to_balancing(adm)
    adm.drain()
    await adm.send({"t": "cfg_set", "key": "limit.v_max", "value": 0.4})
    assert (await adm.recv("err"))["code"] == "UNSAFE_WHILE_BALANCING"
    await adm.send({"t": "cfg_set", "key": "pid.angle.kp", "value": 55})
    assert (await adm.recv("cfg"))["status"] == "ok"  # tuning keys are fine
    await adm.send({"t": "disarm"})
    assert await wait_until(lambda: rig.sup.state == P.ST_STANDBY, 2.0)
    await adm.send({"t": "cfg_set", "key": "limit.v_max", "value": 0.4})
    assert (await adm.recv("cfg"))["status"] == "ok"


async def test_cfg_times_out_when_the_robot_does_not_answer(rig):
    adm = rig.client("adm", Role.ADMIN)
    adm.drain()
    rig.fake.hang()
    await adm.send({"t": "cfg_get", "key": "pid.angle.kp"})
    assert (await adm.recv("err", timeout=2.0))["code"] == "TIMEOUT"
    assert not rig.sup._pending_cfg  # no leaked futures


async def test_cfg_firmware_rejection_is_reported(rig):
    adm = rig.client("adm", Role.ADMIN)
    adm.drain()
    # balbotd's own range is wider than the (fake) firmware's for this key: the firmware has the last word
    rig.fake.cfg_values[1] = 60.0
    import balbotd.cfgkeys as ck
    orig = ck.BY_NAME["pid.angle.kp"]
    try:
        ck.BY_NAME["pid.angle.kp"] = ck.Key(1, "pid.angle.kp", 60.0, 0.0, 1000.0)
        await adm.send({"t": "cfg_set", "key": "pid.angle.kp", "value": 500.0})
        r = await adm.recv("cfg")
    finally:
        ck.BY_NAME["pid.angle.kp"] = orig
    assert r["status"] == "out_of_range" and r["value"] == pytest.approx(60.0)


# ---- telemetry --------------------------------------------------------------------------------------
async def test_telemetry_is_decimated_per_client(rig):
    fast, slow, off = rig.client("fast", Role.VIEWER), rig.client("slow", Role.VIEWER), rig.client("off", Role.VIEWER)
    await fast.send({"t": "sub", "rate": 50})
    await slow.send({"t": "sub", "rate": 5})
    await off.send({"t": "sub", "rate": 0})
    for x in (fast, slow, off):
        x.drain()
    await asyncio.sleep(1.0)
    n = {k: sum(1 for m in x.drain() if m["t"] == "tlm") for k, x in (("fast", fast), ("slow", slow), ("off", off))}
    assert 35 <= n["fast"] <= 55 and 3 <= n["slow"] <= 7 and n["off"] == 0, n


async def test_telemetry_message_content(rig):
    c = rig.client("v", Role.VIEWER)
    await c.send({"t": "sub", "rate": 20})
    t = await c.recv("tlm", 1.0)
    assert t["state"] in P.STATES and math.isclose(t["vbat"], 11.1) and set(t) >= {"pitch", "v", "uL", "uR", "yaw_rate"}


async def test_state_changes_are_pushed(rig):
    c = rig.client("v", Role.VIEWER)
    await c.recv("state", 3.0, state="STANDBY")
    adm = rig.client("adm", Role.ADMIN)
    await adm.send({"t": "estop"})
    st = await c.recv("state", 2.0, state="FAULT")
    assert "ESTOP" in st["fault_names"]


# ---- robustness --------------------------------------------------------------------------------------
async def test_bad_input_gets_errors_not_crashes(rig):
    c = rig.client()
    c.drain()
    for text, code in (("not json", "BAD_JSON"), ("[1,2]", "BAD_MESSAGE"), ('{"t":5}', "BAD_MESSAGE"),
                       ('{"x":1}', "BAD_MESSAGE"), ('{"t":"bogus"}', "UNKNOWN_TYPE"),
                       ('{"t":"__class__"}', "UNKNOWN_TYPE"), ('{"t":"_err"}', "UNKNOWN_TYPE"),
                       ('{"t":"handle"}', "UNKNOWN_TYPE"), ("[" * 2000 + "]" * 2000, "BAD_JSON")):
        await c.send_raw(text)
        assert (await c.recv("err"))["code"] == code, text[:20]


async def test_drive_argument_validation(rig):
    c = rig.client()
    await c.lease()
    for text in ('{"t":"drive","seq":1,"vx":NaN,"wz":0}', '{"t":"drive","seq":1,"vx":"1","wz":0}',
                 '{"t":"drive","seq":-1,"vx":0,"wz":0}', '{"t":"drive","vx":0,"wz":0}',
                 '{"t":"drive","seq":1,"vx":true,"wz":0}', '{"t":"drive","seq":1,"vx":0,"wz":0,"flags":"x"}',
                 '{"t":"drive","seq":1,"vx":Infinity,"wz":0}'):
        c.drain()
        await c.send_raw(text)
        assert (await c.recv("err"))["code"] == "BAD_ARG", text
    assert rig.sup.stats.drive_dropped_seq == 0


async def test_oversized_message_and_rate_limit_disconnect(rig):
    c = rig.client()
    with pytest.raises(ClientViolation, match="large"):
        await c.send_raw('{"t":"ping","pad":"' + "x" * 5000 + '"}')
    c2 = rig.client("flood")
    with pytest.raises(ClientViolation, match="rate"):
        for _ in range(1000):
            await c2.send({"t": "ping", "ts": 1})
    assert rig.sup.stats.violations >= 2


async def test_duplicate_and_old_sequence_numbers_are_dropped(rig):
    c = rig.client()
    await rig.to_balancing(c)
    base = {"t": "drive", "vx": 1.0, "wz": 0.0}
    await c.send({**base, "seq": 10})
    await asyncio.sleep(0.05)
    assert rig.fake._v_target > 0
    await c.send({**base, "seq": 9, "vx": -1.0})   # older: dropped, must not reverse the robot
    await c.send({**base, "seq": 10, "vx": -1.0})  # duplicate
    await asyncio.sleep(0.06)
    assert rig.fake._v_target > 0 and rig.sup.stats.drive_dropped_seq == 2
    await c.send({**base, "seq": 11, "vx": -1.0})
    await asyncio.sleep(0.06)
    assert rig.fake._v_target < 0


async def test_stale_timestamps_are_dropped_after_offset_estimation(rig):
    c = rig.client()
    await rig.to_balancing(c)
    now_ms = int((time.monotonic() - c.t0) * 1000)
    await c.send({"t": "drive", "seq": 100, "ts": now_ms, "vx": 1.0, "wz": 0})
    await asyncio.sleep(0.06)
    assert rig.fake._v_target > 0
    # a burst that sat in a queue for 600 ms: its timestamp is far older than the freshest one seen
    await c.send({"t": "drive", "seq": 101, "ts": now_ms - 600, "vx": -1.0, "wz": 0})
    await asyncio.sleep(0.06)
    assert rig.sup.stats.drive_dropped_stale == 1 and rig.fake._v_target > 0


async def test_slow_client_is_cut_off_without_hurting_others():
    cfg = Config(limits=Limits(queue_size=8))
    r = Rig(cfg, FakeConfig())
    r.fake.start()
    r.sup.start()
    try:
        slow, ok = r.client("slow", Role.VIEWER), r.client("ok", Role.VIEWER)
        await slow.send({"t": "sub", "rate": 50})
        await ok.send({"t": "sub", "rate": 50})
        for _ in range(30):  # `ok` keeps reading, `slow` never does
            await asyncio.sleep(0.03)
            ok.drain()
        assert slow.c.closed and not ok.c.closed
        assert r.sup.stats.violations >= 1
        assert any(m["t"] == "tlm" for m in ok.drain() + [await ok.recv("tlm", 1.0)])
    finally:
        await r.sup.stop()
        await r.fake.stop()


# ---- the M4F side of the link ---------------------------------------------------------------------------
async def test_m4f_liveness_is_tracked_and_published(rig):
    v = rig.client("v", Role.VIEWER)
    assert await wait_until(lambda: rig.sup.m4f_alive, 2.0)
    rig.fake.hang()
    st = await v.recv("state", 2.0, m4f_alive=False)
    assert "M4F_DEAD" in st["fault_names"]
    rig.fake.resume()
    st = await v.recv("state", 2.0, m4f_alive=True)
    assert "M4F_DEAD" not in st["fault_names"]


async def test_garbage_and_corrupt_frames_from_the_m4f_are_counted(rig):
    await rig.b.send(b"\x01\x02\x03")
    await rig.b.send(b"\xb5\x01" + b"\x00" * 20)
    await asyncio.sleep(0.05)
    assert rig.sup.stats.crc_errors >= 2 and rig.sup.m4f_alive


async def test_bad_frames_do_not_refresh_liveness(rig):
    assert await wait_until(lambda: rig.sup.m4f_alive, 2.0)
    rig.fake.hang()
    end = time.monotonic() + 0.8
    while time.monotonic() < end:  # garbage keeps arriving, but no valid frame
        await rig.b.send(b"\x00" * 12)
        await asyncio.sleep(0.05)
    assert not rig.sup.m4f_alive


async def test_lossy_link_keeps_working(rig):
    import random
    rng = random.Random(3)
    rig.a.drop_filter = lambda d: P.decode(d).type in (P.CMD_DRIVE, P.CMD_HEARTBEAT) and rng.random() < 0.3
    rig.b.drop_filter = lambda d: rng.random() < 0.3
    c = rig.client()
    await rig.to_balancing(c)
    for _ in range(40):
        await c.drive(0.7)
        await asyncio.sleep(0.02)
    assert rig.fake.v > 0.1 and rig.fake.state == P.ST_BALANCING
    assert rig.sup.stats.seq_gaps > 0  # losses were noticed


async def test_drive_and_heartbeat_rates(rig):
    await asyncio.sleep(1.0)
    f0, h0 = rig.fake.drive_frames, rig.fake.rx_frames
    await asyncio.sleep(1.0)
    drives = rig.fake.drive_frames - f0
    total = rig.fake.rx_frames - h0
    assert 40 <= drives <= 55, drives          # 50 Hz CMD_DRIVE even with nobody driving (zeros)
    assert 8 <= total - drives <= 13, total    # 10 Hz CMD_HEARTBEAT


async def test_reconnect_after_the_link_dies():
    cfg = Config()
    ends = []

    async def factory():
        a, b = MemoryLink.pair()
        ends.append((a, b))
        from balbotd.fakem4 import FakeM4F
        f = FakeM4F(b, FakeConfig(boot_s=0.02, cal_s=0.05))
        f.start()
        ends[-1] = (a, b, f)
        return a

    sup = Supervisor(None, cfg, link_factory=factory)
    sup.start()
    try:
        assert await wait_until(lambda: sup.m4f_alive, 2.0)
        assert sup.stats.reconnects == 1
        ends[0][0].close()  # the rpmsg device goes away (M4F firmware restarted)
        assert await wait_until(lambda: sup.stats.reconnects == 2 and sup.m4f_alive, 4.0)
    finally:
        await sup.stop()
        for e in ends:
            await e[2].stop()


# ---- fault reset ---------------------------------------------------------------------------------------------
async def test_reset_recovers_from_an_estop_and_needs_the_lease(rig):
    d, v = rig.client("d"), rig.client("v", Role.VIEWER)
    await rig.to_balancing(d)
    await d.send({"t": "estop"})
    assert await wait_until(lambda: rig.fake.state == P.ST_FAULT, 1.0)
    await v.send({"t": "reset"})
    assert (await v.recv("err"))["code"] == "NOT_DRIVER"
    assert rig.fake.state == P.ST_FAULT
    await asyncio.sleep(0.08)  # let the two redundant ESTOP resends (15 ms, 30 ms) land: a reset racing them would lose, safely
    await d.send({"t": "ping", "ts": 1})  # a real client keeps pinging; the 1 s lease would otherwise have lapsed
    await d.send({"t": "reset"})
    assert await wait_until(lambda: rig.fake.state == P.ST_STANDBY, 3.0)
    assert rig.fake.faults == 0
    st = await d.recv("state", 2.0, state="STANDBY")
    assert st["fault_names"] == []


async def test_reset_is_refused_while_the_hardware_estop_is_still_pressed(rig):
    adm = rig.client("adm", Role.ADMIN)
    assert await wait_until(lambda: rig.fake.state == P.ST_STANDBY, 3.0)
    rig.fake.press_estop(True)
    assert await wait_until(lambda: rig.fake.state == P.ST_FAULT, 1.0)
    await adm.send({"t": "reset"})
    await asyncio.sleep(0.2)
    assert rig.fake.state == P.ST_FAULT and rig.fake.refused >= 1
    rig.fake.press_estop(False)
    await adm.send({"t": "reset"})
    assert await wait_until(lambda: rig.fake.state == P.ST_STANDBY, 3.0)


async def test_reset_is_sent_once_so_a_late_duplicate_cannot_clear_a_fresh_fault(rig):
    adm = rig.client("adm", Role.ADMIN)
    assert await wait_until(lambda: rig.fake.state == P.ST_STANDBY, 3.0)
    seen = []
    orig = rig.a.drop_filter
    rig.a.drop_filter = lambda d: (seen.append(P.decode(d).type) or False)
    await adm.send({"t": "estop"})
    assert await wait_until(lambda: rig.fake.state == P.ST_FAULT, 1.0)
    await asyncio.sleep(0.08)
    await adm.send({"t": "reset"})
    await asyncio.sleep(0.2)
    assert seen.count(P.CMD_RESET) == 1 and seen.count(P.CMD_ESTOP) == 3


async def test_repeated_estops_from_the_page_are_coalesced_but_a_later_one_still_goes_out(rig):
    c = rig.client("v", Role.VIEWER)
    seen = []
    rig.a.drop_filter = lambda d: (seen.append(P.decode(d).type) or False)
    for _ in range(3):  # what the page does: now, +15 ms, +30 ms
        await c.send({"t": "estop"})
        await asyncio.sleep(0.015)
    await asyncio.sleep(0.15)
    assert seen.count(P.CMD_ESTOP) == 3, seen.count(P.CMD_ESTOP)
    await c.send({"t": "estop"})  # a genuinely new press later
    await asyncio.sleep(0.15)
    assert seen.count(P.CMD_ESTOP) == 6
