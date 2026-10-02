# SPDX-License-Identifier: GPL-2.0-only
"""Closed-loop simulation tests SIM-1..: the real C controller (libbalbot.so) against the
nonlinear plant with IMU noise, gyro bias, quantisation, encoder quantisation and one control
period of actuation delay (docs/balance-robot/06-test-plan-ci.md, 6.4).

Needs libbalbot.so: cmake -S robot/core -B robot/core/build-shared -DBUILD_SHARED=ON && cmake --build ... --target balbot
(or set BALBOT_LIB). Set BALBOT_MC=<n> to change the Monte-Carlo sample count (default 12).
"""
import dataclasses
import math
import os

import numpy as np
import pytest

from plant import Controller, Params, Plant, run

LIB = os.environ.get("BALBOT_LIB") or os.path.join(os.path.dirname(__file__), "..", "core", "build-shared", "libbalbot.so")
pytestmark = pytest.mark.skipif(not os.path.exists(LIB), reason="libbalbot.so not built")

# defaults of bb_params_default() plus the estimator settings used by the firmware skeleton
CF_TAU = 4.0
CF_GATE = 0.03
GAINS = (60.0, 0.0, 7.5, 0.35, 0.017, 1.0, 2.0)
KV_FF = 10.2
DT = 0.002


def make(P=None, theta0=0.0, seed=1, cal_err=0.1, use_kf=False, gains=GAINS, kv_ff=KV_FF):
    P = P or Params()
    plant = Plant(P, np.random.default_rng(seed), theta0)
    c = Controller(LIB, use_kf, cf_tau=CF_TAU, gate_g=CF_GATE)
    c.configure(gains, u_dz=0.8 * P.u_start, kv_ff=kv_ff)
    c.set_gyro_bias(P.gyro_bias_y * (1.0 - cal_err), P.gyro_bias_z)  # calibration leaves 10 % residual
    return plant, c


def deg(a):
    return np.degrees(np.asarray(a))


@pytest.mark.parametrize("tilt_deg", [5.0, -5.0, 10.0])
def test_sim1_recovers_from_initial_tilt(tilt_deg):
    plant, c = make(theta0=math.radians(tilt_deg))
    tr = run(c, plant, 5.0)
    th = deg(tr.theta)
    t = np.array(tr.t)
    assert not tr.fell
    idx = np.nonzero(np.abs(th) > 0.5)[0]
    settle = t[idx[-1]] if len(idx) else 0.0
    assert settle < 2.5, f"settled after {settle:.2f}s"
    assert abs(tr.x[-1]) < 0.3


@pytest.mark.parametrize("dv", [0.3, 0.5, 0.8])
def test_sim2_push_recovery(dv):
    plant, c = make()
    tr = run(c, plant, 6.0, push=lambda t: dv if abs(t - 1.0) < 1e-6 else 0.0)
    assert not tr.fell
    assert np.abs(deg(tr.theta)).max() < 15.0
    assert abs(tr.x[-1]) < 0.8


def test_sim2_strong_push_is_beyond_the_limit_and_falls():
    """Documents the limit: ~1.0 m/s kick exceeds the voltage headroom of this robot at 11.1 V."""
    plant, c = make()
    tr = run(c, plant, 5.0, push=lambda t: 1.2 if abs(t - 1.0) < 1e-6 else 0.0)
    assert tr.fell


def test_sim4_speed_step():
    plant, c = make()
    tr = run(c, plant, 12.0, intent=lambda t: (1.0 if 1.0 <= t < 4.0 else 0.0, 0.0))
    v = np.array(tr.v)
    assert not tr.fell
    assert v.max() < 0.6 * 1.25, f"overshoot {(v.max() / 0.6 - 1) * 100:.0f}%"
    assert np.abs(deg(tr.theta)).max() < 10.0
    assert abs(v[int(3.9 / DT)] - 0.6) < 0.06
    assert abs(v[-1]) < 0.08


def test_sim4_reverse_speed():
    plant, c = make()
    tr = run(c, plant, 8.0, intent=lambda t: (-0.7 if 1.0 <= t < 4.0 else 0.0, 0.0))
    assert not tr.fell
    assert np.abs(deg(tr.theta)).max() < 10.0


def test_sim5_yaw_step_while_balancing():
    plant, c = make()
    tr = run(c, plant, 6.0, intent=lambda t: (0.0, 0.8 if 1.0 <= t < 4.0 else 0.0))
    pd = np.array(tr.psi_dot)
    assert not tr.fell
    target = 0.8 * 2.5
    assert abs(pd[int(3.5 / DT)] - target) < 0.1 * target
    assert np.abs(deg(tr.theta)).max() < 5.0
    assert abs(tr.x[-1]) < 0.5


def test_sim5_drive_and_turn_together():
    plant, c = make()
    tr = run(c, plant, 8.0, intent=lambda t: (0.5, 0.4 if t > 2.0 else 0.0))
    assert not tr.fell
    assert np.abs(deg(tr.theta)).max() < 10.0


def test_sim9_battery_sag_during_run():
    plant, c = make()
    tr = run(c, plant, 10.0, intent=lambda t: (0.5 if 1.0 <= t < 8.0 else 0.0, 0.0),
             vbat=lambda t: 12.6 - (12.6 - 9.0) * min(1.0, t / 10.0))
    assert not tr.fell


def test_sim8_gyro_bias_step_is_rejected_by_calibration_error_margin():
    """A 30 % calibration error (instead of 10 %) must still balance, with a visible lean/drift."""
    plant, c = make(cal_err=0.3)
    tr = run(c, plant, 8.0)
    assert not tr.fell
    assert np.abs(deg(tr.theta[-250:])).max() < 3.0


def test_delay_margin_nominal_has_headroom():
    """SIM-7: with 3 extra control periods (6 ms) of actuation delay the nominal design still balances."""
    plant, c = make(theta0=math.radians(3.0))
    tr = run(c, plant, 5.0, delay_steps=4)
    assert not tr.fell


def test_sim_without_deadband_compensation_survives_realistic_stiction():
    P = Params(u_start=0.8)
    plant, c = make(P, theta0=math.radians(3.0))
    c.configure(GAINS, u_dz=0.0, kv_ff=KV_FF)
    tr = run(c, plant, 6.0)
    assert not tr.fell


def test_sim6_monte_carlo_parameter_robustness():
    n = int(os.environ.get("BALBOT_MC", "12"))
    rng = np.random.default_rng(42)
    ok = 0
    for i in range(n):
        P = dataclasses.replace(
            Params(),
            m=0.9 * rng.uniform(0.75, 1.25), l=0.12 * rng.uniform(0.75, 1.25), I=0.004 * rng.uniform(0.7, 1.3),
            Jr=3e-6 * rng.uniform(0.7, 1.3), Kt=0.33 * rng.uniform(0.85, 1.15), Ke=0.33 * rng.uniform(0.85, 1.15),
            R=5.5 * rng.uniform(0.8, 1.2), u_start=rng.uniform(0.0, 0.8), imu_height=rng.uniform(0.0, 0.12),
            gyro_bias_y=rng.uniform(-0.03, 0.03), vbat=rng.uniform(10.0, 12.6))
        plant, c = make(P, theta0=math.radians(rng.uniform(-4, 4)), seed=i)
        tr = run(c, plant, 5.0, push=lambda t: 0.4 if abs(t - 2.0) < 1e-6 else 0.0, vbat=lambda t, v=P.vbat: v)
        ok += not tr.fell
    assert ok >= int(0.9 * n + 0.5), f"only {ok}/{n} stayed up"


def test_defaults_match_lqr_design():
    """The numbers in bb_params_default() must be what lqr_design.py produces for the example model."""
    from lqr_design import design
    d = design(Params())
    assert abs(d["Kp"] - GAINS[0]) / d["Kp"] < 0.05
    assert abs(d["Kd"] - GAINS[2]) / d["Kd"] < 0.05
    assert abs(d["Kvp"] - GAINS[3]) / d["Kvp"] < 0.05
    assert abs(d["Kvi"] - GAINS[4]) / d["Kvi"] < 0.10
    assert abs(d["kv_ff"] - KV_FF) < 0.1
    assert max(np.real(d["poles_open"])) > 5.0       # unstable pendulum pole, rad/s
    assert max(np.real(d["poles_closed"])) < 0.0     # stabilised
