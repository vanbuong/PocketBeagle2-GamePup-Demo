# SPDX-License-Identifier: GPL-2.0-only
"""Nonlinear wheeled-inverted-pendulum plant + sensor models (docs/balance-robot/05, 5.1).

Frame/sign conventions follow core/include/balbot/conventions.h: pitch > 0 = leaning forward,
positive motor volts drive the robot forward, yaw > 0 = turning left.
"""
import ctypes
import math
import os
from dataclasses import dataclass, field

import numpy as np

G = 9.81
G0 = 9.80665


@dataclass
class Params:
    # body
    m: float = 0.9          # kg
    l: float = 0.12         # m, axle to CG
    I: float = 0.004        # kg m^2, pitch inertia about the CG
    Iz: float = 0.006       # kg m^2, yaw inertia of the body
    track: float = 0.15     # m
    # wheels / drivetrain
    r: float = 0.0325       # m
    mw: float = 0.05        # kg per wheel
    Jr: float = 3e-6        # kg m^2 rotor inertia (motor side)
    N: float = 30.0
    Kt: float = 0.33        # N m/A at the output shaft
    Ke: float = 0.33        # V s/rad at the output shaft
    R: float = 5.5          # ohm incl. TB6612 on-resistance
    u_start: float = 0.0    # V: stiction / dead band
    c_visc: float = 0.0     # N s/m viscous drag on the base
    # sensing
    imu_height: float = 0.05    # m above the axle
    gyro_noise: float = 0.0024  # rad/s rms (0.01 dps/rtHz at 184 Hz)
    gyro_bias_y: float = 0.02   # rad/s
    gyro_bias_z: float = 0.0
    accel_noise: float = 0.0044  # g rms
    counts_per_m: float = 1320.0 / (math.pi * 0.065)
    # supply
    vbat: float = 11.1

    @property
    def m_eff(self) -> float:
        """Both wheels plus reflected rotor inertia, as an equivalent translating mass."""
        jw = 0.5 * self.mw * self.r ** 2
        return 2 * self.mw + 2 * (jw + self.Jr * self.N ** 2) / self.r ** 2


def _dz(x: float, d: float) -> float:
    if d <= 0.0:
        return x
    if abs(x) <= d:
        return 0.0
    return x - math.copysign(d, x)


class Plant:
    """State: x, v, theta, omega, psi, psi_dot (RK4, internal step 0.1 ms)."""

    def __init__(self, p: Params, rng: np.random.Generator, theta0: float = 0.0):
        self.p = p
        self.rng = rng
        self.s = np.array([0.0, 0.0, theta0, 0.0, 0.0, 0.0])
        self.last_acc = (0.0, 0.0)  # xdd, thetadd for the accelerometer model
        self.u = (0.0, 0.0)

    def _deriv(self, s, uL, uR):
        p = self.p
        x, v, th, om, psi, pd = s
        M = p.m_eff
        # wheel speeds (ground speed of each wheel), left wheel is slower when turning left
        vl = v - 0.5 * p.track * pd
        vr = v + 0.5 * p.track * pd
        k = p.Kt / (p.R * p.r)
        Fl = k * _dz(uL - p.Ke * vl / p.r, p.u_start)
        Fr = k * _dz(uR - p.Ke * vr / p.r, p.u_start)
        F = Fl + Fr - p.c_visc * v
        # pendulum: [(M+m), m l cos; m l cos, (I+m l^2)] [xdd, thdd] = [F + m l om^2 sin, m g l sin]
        c, sn = math.cos(th), math.sin(th)
        a11, a12 = M + p.m, p.m * p.l * c
        a21, a22 = p.m * p.l * c, p.I + p.m * p.l ** 2
        b1 = F + p.m * p.l * om * om * sn
        b2 = p.m * G * p.l * sn
        det = a11 * a22 - a12 * a21
        xdd = (b1 * a22 - a12 * b2) / det
        thdd = (a11 * b2 - a21 * b1) / det
        Iz = p.Iz + M * (0.5 * p.track) ** 2
        pdd = (Fr - Fl) * 0.5 * p.track / Iz
        return np.array([v, xdd, om, thdd, pd, pdd]), xdd, thdd

    def step(self, uL, uR, dt, h=1e-4):
        n = max(1, int(round(dt / h)))
        hh = dt / n
        s = self.s
        for _ in range(n):
            k1, xdd, thdd = self._deriv(s, uL, uR)
            k2, _, _ = self._deriv(s + 0.5 * hh * k1, uL, uR)
            k3, _, _ = self._deriv(s + 0.5 * hh * k2, uL, uR)
            k4, _, _ = self._deriv(s + hh * k3, uL, uR)
            s = s + hh / 6.0 * (k1 + 2 * k2 + 2 * k3 + k4)
        self.s = s
        self.last_acc = (xdd, thdd)
        self.u = (uL, uR)

    # ---- sensors -------------------------------------------------------------------
    def imu_raw(self):
        """int16 MPU-6500 words (accel +-4 g, gyro +-1000 dps) with noise, bias, quantisation."""
        p = self.p
        x, v, th, om, psi, pd = self.s
        xdd, thdd = self.last_acc
        d = p.imu_height
        axw = xdd + d * (thdd * math.cos(th) - om * om * math.sin(th))
        azw = d * (-thdd * math.sin(th) - om * om * math.cos(th))
        fx = axw * math.cos(th) - (azw + G) * math.sin(th)
        fz = axw * math.sin(th) + (azw + G) * math.cos(th)
        ax = fx / G0 + self.rng.normal(0, p.accel_noise)
        az = fz / G0 + self.rng.normal(0, p.accel_noise)
        ay = self.rng.normal(0, p.accel_noise)
        gy = om + p.gyro_bias_y + self.rng.normal(0, p.gyro_noise)
        gz = pd + p.gyro_bias_z + self.rng.normal(0, p.gyro_noise)
        gx = self.rng.normal(0, p.gyro_noise)

        def q(val, lsb, lim=32767):
            return int(max(-32768, min(lim, round(val * lsb))))

        dps = 180.0 / math.pi
        return [q(ax, 8192.0), q(ay, 8192.0), q(az, 8192.0), 0,
                q(gx * dps, 32.8), q(gy * dps, 32.8), q(gz * dps, 32.8)]

    def encoders(self):
        """Wheel counts (4x quadrature), integer, from wheel travel."""
        p = self.p
        x, v, th, om, psi, pd = self.s
        xl = x - 0.5 * p.track * psi
        xr = x + 0.5 * p.track * psi
        return int(math.floor(xl * p.counts_per_m)), int(math.floor(xr * p.counts_per_m))


class Controller:
    """ctypes wrapper around libbalbot.so (the real core code + sim/bridge.c)."""

    def __init__(self, lib_path: str | None = None, use_kf: bool = False, counts_per_m: float = 1320.0 / (math.pi * 0.065),
                 cf_tau: float = 1.0, gate_g: float = 0.1):
        lib_path = lib_path or os.environ.get("BALBOT_LIB") or os.path.join(
            os.path.dirname(__file__), "..", "core", "build-shared", "libbalbot.so")
        self.lib = ctypes.CDLL(lib_path)
        self.lib.bbsim_create.restype = ctypes.c_void_p
        self.lib.bbsim_create.argtypes = [ctypes.c_int, ctypes.c_double, ctypes.c_double, ctypes.c_double]
        self.lib.bbsim_destroy.argtypes = [ctypes.c_void_p]
        self.lib.bbsim_configure.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_double), ctypes.c_double, ctypes.c_double, ctypes.c_double]
        self.lib.bbsim_set_gyro_bias.argtypes = [ctypes.c_void_p, ctypes.c_double, ctypes.c_double]
        self.lib.bbsim_set_intent.argtypes = [ctypes.c_void_p, ctypes.c_double, ctypes.c_double]
        self.lib.bbsim_reset_ctrl.argtypes = [ctypes.c_void_p]
        self.lib.bbsim_set_theta_trim.argtypes = [ctypes.c_void_p, ctypes.c_double]
        self.lib.bbsim_step.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int16), ctypes.c_int32, ctypes.c_int32,
                                        ctypes.c_double, ctypes.c_double, ctypes.POINTER(ctypes.c_double)]
        self.h = self.lib.bbsim_create(1 if use_kf else 0, 1.0 / counts_per_m, cf_tau, gate_g)
        self._out = (ctypes.c_double * 8)()
        self._raw = (ctypes.c_int16 * 7)()

    def close(self):
        if self.h:
            self.lib.bbsim_destroy(self.h)
            self.h = None

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def configure(self, gains, u_dz=0.0, theta_max_deg=10.0, kv_ff=0.0):
        g = (ctypes.c_double * 7)(*gains)
        self.lib.bbsim_configure(self.h, g, u_dz, math.radians(theta_max_deg), kv_ff)

    def set_gyro_bias(self, by, bz):
        self.lib.bbsim_set_gyro_bias(self.h, by, bz)

    def set_intent(self, vx, wz):
        self.lib.bbsim_set_intent(self.h, vx, wz)

    def set_theta_trim(self, t):
        self.lib.bbsim_set_theta_trim(self.h, t)

    def step(self, raw, counts, vbat, dt):
        for i, v in enumerate(raw):
            self._raw[i] = v
        self.lib.bbsim_step(self.h, self._raw, counts[0], counts[1], vbat, dt, self._out)
        return list(self._out)


@dataclass
class Trace:
    t: list = field(default_factory=list)
    theta: list = field(default_factory=list)
    est_pitch: list = field(default_factory=list)
    v: list = field(default_factory=list)
    x: list = field(default_factory=list)
    psi_dot: list = field(default_factory=list)
    uL: list = field(default_factory=list)
    uR: list = field(default_factory=list)
    theta_ref: list = field(default_factory=list)
    fell: bool = False


def run(ctrl: Controller, plant: Plant, t_end: float, dt: float = 0.002, intent=None, push=None, vbat=None,
        delay_steps: int = 1, tip_deg: float = 45.0) -> Trace:
    """Closed loop at 500 Hz. intent(t)->(vx,wz); push(t)->(delta_v) one-shot velocity kick of the base;
    vbat(t)->volts. delay_steps adds actuation delay (one step = one control period)."""
    tr = Trace()
    n = int(t_end / dt)
    queue = [(0.0, 0.0)] * delay_steps
    for k in range(n):
        t = k * dt
        if intent:
            ctrl.set_intent(*intent(t))
        if push:
            dv = push(t)
            if dv:
                plant.s[1] += dv
        vb = vbat(t) if vbat else plant.p.vbat
        out = ctrl.step(plant.imu_raw(), plant.encoders(), vb, dt)
        queue.append((out[0], out[1]))
        uL, uR = queue.pop(0)
        plant.step(uL, uR, dt)
        s = plant.s
        tr.t.append(t)
        tr.theta.append(s[2])
        tr.est_pitch.append(out[2])
        tr.v.append(s[1])
        tr.x.append(s[0])
        tr.psi_dot.append(s[5])
        tr.uL.append(uL)
        tr.uR.append(uR)
        tr.theta_ref.append(out[4])
        if abs(s[2]) > math.radians(tip_deg):
            tr.fell = True
            break
    return tr
