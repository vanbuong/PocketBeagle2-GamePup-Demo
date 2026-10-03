# SPDX-License-Identifier: GPL-2.0-only
"""Configuration keys exchanged with the M4F (CFG_SET / CFG_GET).

The ranges here are a first filter in balbotd; the M4F firmware is the authority and rejects out-of-range
values itself. `safety` keys may only change while the robot is not balancing.
"""
from dataclasses import dataclass


@dataclass(frozen=True)
class Key:
    id: int
    name: str
    default: float
    lo: float
    hi: float
    safety: bool = False


KEYS = [
    Key(1, "pid.angle.kp", 60.0, 0.0, 300.0),
    Key(2, "pid.angle.ki", 0.0, 0.0, 50.0),
    Key(3, "pid.angle.kd", 7.5, 0.0, 40.0),
    Key(4, "pid.speed.kp", 0.35, 0.0, 2.0),
    Key(5, "pid.speed.ki", 0.017, 0.0, 0.5),
    Key(6, "pid.yaw.kp", 1.0, 0.0, 10.0),
    Key(7, "pid.yaw.ki", 2.0, 0.0, 20.0),
    Key(8, "ff.speed", 10.2, 0.0, 30.0),
    Key(16, "limit.v_max", 0.6, 0.0, 1.0, True),
    Key(17, "limit.w_max", 2.5, 0.0, 6.0, True),
    Key(18, "limit.a_max", 0.8, 0.1, 2.0, True),
    Key(19, "limit.theta_max_deg", 10.0, 2.0, 20.0, True),
    Key(20, "trim.theta_deg", 0.0, -10.0, 10.0),
    Key(32, "battery.low_v", 9.9, 8.0, 12.0, True),
    Key(33, "tip.deg", 35.0, 15.0, 60.0, True),
]
BY_NAME = {k.name: k for k in KEYS}
BY_ID = {k.id: k for k in KEYS}
