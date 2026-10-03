# SPDX-License-Identifier: GPL-2.0-only
"""balbotd.toml loading (see balbotd.example.toml)."""
from __future__ import annotations

import tomllib
from dataclasses import dataclass, field

from .arbiter import Role
from .auth import TokenEntry
from .shaping import ShapingConfig


@dataclass
class Timing:
    drive_period_s: float = 0.020      # CMD_DRIVE rate
    heartbeat_period_s: float = 0.100  # CMD_HEARTBEAT rate
    drive_failsafe_s: float = 0.250    # no drive from the driver -> zero velocity
    lease_timeout_s: float = 1.0
    m4f_dead_s: float = 0.500          # no valid frame from the M4F
    cfg_reply_timeout_s: float = 0.5
    max_drive_age_s: float = 0.250     # drop drive frames older than this (after offset estimation)


@dataclass
class Limits:
    max_message_bytes: int = 4096
    max_msgs_per_s: int = 200
    queue_size: int = 256              # per-client outgoing queue; slow clients are disconnected


@dataclass
class VideoConfig:
    mode: str = "auto"           # auto | off | pattern | camera | command ; auto = pattern with --fake-m4, else off
    device: str = "/dev/video0"  # camera mode
    size: str = "1280x720"
    fps: int = 30
    command: list = field(default_factory=list)  # command mode: argv writing concatenated JPEGs to stdout
    idle_stop_s: float = 3.0     # stop the source this long after the last viewer left


@dataclass
class Config:
    shaping: ShapingConfig = field(default_factory=ShapingConfig)
    timing: Timing = field(default_factory=Timing)
    limits: Limits = field(default_factory=Limits)
    video: VideoConfig = field(default_factory=VideoConfig)
    tokens: list[TokenEntry] = field(default_factory=list)


def _section(cls, data: dict):
    known = {f for f in cls.__dataclass_fields__}
    unknown = set(data) - known
    if unknown:
        raise ValueError(f"unknown keys in [{cls.__name__.lower()}]: {sorted(unknown)}")
    return cls(**data)


def parse(data: dict) -> Config:
    cfg = Config()
    if "shaping" in data:
        cfg.shaping = _section(ShapingConfig, data["shaping"])
    if "timing" in data:
        cfg.timing = _section(Timing, data["timing"])
    if "limits" in data:
        cfg.limits = _section(Limits, data["limits"])
    if "video" in data:
        cfg.video = _section(VideoConfig, data["video"])
        if cfg.video.mode not in ("auto", "off", "pattern", "camera", "command"):
            raise ValueError("video.mode must be auto, off, pattern, camera or command")
        if cfg.video.mode == "command" and not (cfg.video.command and all(isinstance(a, str) for a in cfg.video.command)):
            raise ValueError("video.command must be a non-empty list of strings in command mode")
    for t in data.get("tokens", []):
        role = {"viewer": Role.VIEWER, "driver": Role.DRIVER, "admin": Role.ADMIN}.get(t.get("role"))
        if role is None:
            raise ValueError(f"token {t.get('name')!r}: role must be viewer, driver or admin")
        h = str(t.get("sha256", ""))
        if len(h) != 64 or any(c not in "0123456789abcdef" for c in h):
            raise ValueError(f"token {t.get('name')!r}: sha256 must be 64 lowercase hex characters")
        cfg.tokens.append(TokenEntry(str(t["name"]), role, h))
    return cfg


def load(path: str) -> Config:
    with open(path, "rb") as f:
        return parse(tomllib.load(f))
