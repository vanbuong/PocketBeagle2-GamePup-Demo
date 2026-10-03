# SPDX-License-Identifier: GPL-2.0-only
"""Turn normalised joystick intent into the physical command sent to the M4F (doc 03, 3.2)."""
import math
from dataclasses import dataclass

INT16_MAX = 32767


@dataclass(frozen=True)
class ShapingConfig:
    v_max: float = 0.6          # m/s at full stick
    w_max: float = 2.5          # rad/s at full stick
    deadband: float = 0.05
    expo_cubic: float = 0.6     # y = c*x^3 + (1-c)*x
    boost_v: float = 1.6        # multiplier with the boost flag
    boost_w: float = 1.0


def _clamp(x: float, lim: float = 1.0) -> float:
    return max(-lim, min(lim, x))


def curve(x: float, deadband: float, cubic: float) -> float:
    """Deadband with rescale (no jump at the edge) followed by the expo curve."""
    x = _clamp(x)
    ax = abs(x)
    if ax <= deadband:
        return 0.0
    y = (ax - deadband) / (1.0 - deadband)
    y = cubic * y ** 3 + (1.0 - cubic) * y
    return math.copysign(y, x)


def shape(vx: float, wz: float, cfg: ShapingConfig, boost: bool = False) -> tuple[int, int]:
    """Returns (v_mm_s, w_mrad_s) as int16-safe integers. Non-finite inputs give zero."""
    if not (math.isfinite(vx) and math.isfinite(wz)):
        return 0, 0
    v = curve(vx, cfg.deadband, cfg.expo_cubic) * cfg.v_max * (cfg.boost_v if boost else 1.0)
    w = curve(wz, cfg.deadband, cfg.expo_cubic) * cfg.w_max * (cfg.boost_w if boost else 1.0)
    v_mm = int(round(max(-INT16_MAX, min(INT16_MAX, v * 1000.0))))
    w_mrad = int(round(max(-INT16_MAX, min(INT16_MAX, w * 1000.0))))
    return v_mm, w_mrad
