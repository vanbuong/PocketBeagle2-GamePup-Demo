# SPDX-License-Identifier: GPL-2.0-only
import math

import pytest

from balbotd import auth, cfgkeys, config
from balbotd.arbiter import Arbiter, Role, Transport
from balbotd.shaping import ShapingConfig, curve, shape


# ---- shaping ----------------------------------------------------------------------------------------------
def test_shape_zero_and_deadband():
    c = ShapingConfig()
    assert shape(0, 0, c) == (0, 0)
    assert shape(0.05, -0.05, c) == (0, 0)
    assert shape(0.06, 0, c)[0] > 0


def test_shape_full_stick_hits_the_limits_and_is_odd_symmetric():
    c = ShapingConfig()
    assert shape(1, 1, c) == (600, 2500)
    assert shape(-1, -1, c) == (-600, -2500)
    assert shape(5, -5, c) == (600, -2500)  # clamped
    for x in (0.1, 0.3, 0.77):
        assert shape(x, x, c) == tuple(-v for v in shape(-x, -x, c))


def test_curve_is_monotonic_and_continuous_at_the_deadband_edge():
    xs = [i / 1000 for i in range(-1000, 1001)]
    ys = [curve(x, 0.05, 0.6) for x in xs]
    assert all(b >= a for a, b in zip(ys, ys[1:]))
    assert abs(curve(0.0501, 0.05, 0.6)) < 0.01  # no jump after the deadband


def test_boost_scales_speed_only_and_never_overflows_int16():
    c = ShapingConfig()
    assert shape(1, 1, c, boost=True) == (960, 2500)
    big = ShapingConfig(v_max=1000.0, w_max=1000.0)
    v, w = shape(1, 1, big, boost=True)
    assert v == 32767 and w == 32767


@pytest.mark.parametrize("bad", [math.nan, math.inf, -math.inf])
def test_non_finite_input_gives_zero(bad):
    assert shape(bad, 0.5, ShapingConfig()) == (0, 0)
    assert shape(0.5, bad, ShapingConfig()) == (0, 0)


# ---- arbiter -----------------------------------------------------------------------------------------------
class Clock:
    def __init__(self):
        self.t = 100.0

    def __call__(self):
        return self.t


def test_first_driver_gets_the_lease_and_epochs_increase():
    clk = Clock()
    a = Arbiter(clk)
    assert a.request("A", Role.DRIVER, Transport.WIFI) == (True, 1, "granted")
    assert a.request("B", Role.DRIVER, Transport.WIFI) == (False, 0, "busy")
    assert a.request("A", Role.DRIVER, Transport.WIFI)[2] == "already"
    assert a.release("A") and a.epoch == 0
    assert a.request("B", Role.ADMIN, Transport.WIFI) == (True, 2, "granted")


def test_viewers_cannot_drive():
    a = Arbiter(Clock())
    assert a.request("V", Role.VIEWER, Transport.WIFI) == (False, 0, "role")
    assert not a.is_driver("V")


def test_lease_expires_after_the_timeout_and_touch_extends_it():
    clk = Clock()
    a = Arbiter(clk, timeout_s=1.0)
    a.request("A", Role.DRIVER, Transport.WIFI)
    clk.t += 0.9
    assert a.touch("A")
    clk.t += 0.9
    assert a.is_driver("A")
    clk.t += 1.1
    assert a.expire() == "A" and not a.is_driver("A")
    assert not a.touch("A")


def test_transport_priority_preempts_but_never_downgrades():
    a = Arbiter(Clock())
    assert a.request("ble", Role.DRIVER, Transport.BLE)[0]
    ok, epoch, why = a.request("wifi", Role.DRIVER, Transport.WIFI)
    assert (ok, why) == (True, "preempted") and epoch == 2
    assert not a.is_driver("ble")
    assert a.request("ble2", Role.DRIVER, Transport.BLE)[2] == "busy"
    assert a.request("phys", Role.ADMIN, Transport.PHYSICAL)[2] == "preempted"
    assert a.request("wifi", Role.DRIVER, Transport.WIFI)[2] == "busy"


def test_epoch_wraps_without_returning_zero():
    a = Arbiter(Clock())
    seen = []
    for i in range(600):
        ok, e, _ = a.request("A", Role.DRIVER, Transport.WIFI)
        seen.append(e)
        a.release("A")
    assert 0 not in seen and max(seen) == 255 and seen[255] == 1


# ---- auth & config ----------------------------------------------------------------------------------------
def test_tokens_roles_and_throttling():
    clk = Clock()
    t_admin, h_admin = auth.new_token()
    t_view, h_view = auth.new_token()
    a = auth.Authenticator([auth.TokenEntry("a", Role.ADMIN, h_admin), auth.TokenEntry("v", Role.VIEWER, h_view)], clk)
    assert a.verify(t_admin, "ip1").role == Role.ADMIN
    assert a.verify(t_view, "ip1").role == Role.VIEWER
    assert a.verify("nope", "ip1") is None and a.verify(None, "ip1") is None
    for _ in range(5):
        a.verify("wrong", "ip2")
    assert a.blocked("ip2") and a.verify(t_admin, "ip2") is None  # even the right token is refused while blocked
    assert not a.blocked("ip1")
    clk.t += 61
    assert a.verify(t_admin, "ip2") is not None


def test_only_the_hash_is_stored():
    t, h = auth.new_token()
    assert len(h) == 64 and t not in h and h == auth.hash_token(t)


def test_config_parse_and_validation():
    h = "a" * 64
    cfg = config.parse({"shaping": {"v_max": 0.4}, "timing": {"drive_failsafe_s": 0.2},
                        "tokens": [{"name": "x", "role": "driver", "sha256": h}]})
    assert cfg.shaping.v_max == 0.4 and cfg.timing.drive_failsafe_s == 0.2 and cfg.tokens[0].role == Role.DRIVER
    with pytest.raises(ValueError, match="unknown keys"):
        config.parse({"timing": {"bogus": 1}})
    with pytest.raises(ValueError, match="role"):
        config.parse({"tokens": [{"name": "x", "role": "root", "sha256": h}]})
    with pytest.raises(ValueError, match="sha256"):
        config.parse({"tokens": [{"name": "x", "role": "admin", "sha256": "xyz"}]})


def test_example_config_file_loads():
    import os
    cfg = config.load(os.path.join(os.path.dirname(__file__), "..", "balbotd.example.toml"))
    assert cfg.timing.drive_failsafe_s == 0.25 and cfg.tokens == []


def test_cfg_keys_are_consistent():
    assert len({k.id for k in cfgkeys.KEYS}) == len(cfgkeys.KEYS) == len(cfgkeys.BY_NAME)
    for k in cfgkeys.KEYS:
        assert k.lo <= k.default <= k.hi
