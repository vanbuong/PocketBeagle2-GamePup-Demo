# SPDX-License-Identifier: GPL-2.0-only
"""Control lease arbitration (doc 03, 3.5): one driver at a time, any number of viewers."""
from __future__ import annotations

import enum
from dataclasses import dataclass
from typing import Callable, Optional


class Role(enum.IntEnum):
    VIEWER = 0
    DRIVER = 1
    ADMIN = 2


class Transport(enum.IntEnum):
    BLE = 1
    WIFI = 2
    PHYSICAL = 3  # priority order: physical > wifi > ble


@dataclass
class Lease:
    holder: str
    transport: Transport
    epoch: int
    last_seen: float


class Arbiter:
    """Not thread safe: use from the supervisor's event loop only."""

    def __init__(self, clock: Callable[[], float], timeout_s: float = 1.0):
        self.clock = clock
        self.timeout_s = timeout_s
        self.lease: Optional[Lease] = None
        self._epoch = 0

    @property
    def epoch(self) -> int:
        """Epoch of the current lease, 0 when nobody drives (the M4F treats 0 as 'no driver')."""
        return self.lease.epoch if self.lease else 0

    def _next_epoch(self) -> int:
        self._epoch = self._epoch % 255 + 1  # 1..255, never 0
        return self._epoch

    def expire(self) -> Optional[str]:
        """Drop the lease if its holder has been silent for timeout_s. Returns the old holder."""
        if self.lease and self.clock() - self.lease.last_seen > self.timeout_s:
            holder = self.lease.holder
            self.lease = None
            return holder
        return None

    def request(self, client: str, role: Role, transport: Transport) -> tuple[bool, int, str]:
        """(granted, epoch, reason). Viewers cannot drive. A higher-priority transport preempts."""
        self.expire()
        if role < Role.DRIVER:
            return False, 0, "role"
        now = self.clock()
        if self.lease is None:
            self.lease = Lease(client, transport, self._next_epoch(), now)
            return True, self.lease.epoch, "granted"
        if self.lease.holder == client:
            self.lease.last_seen = now
            return True, self.lease.epoch, "already"
        if transport > self.lease.transport:
            self.lease = Lease(client, transport, self._next_epoch(), now)
            return True, self.lease.epoch, "preempted"
        return False, 0, "busy"

    def release(self, client: str) -> bool:
        if self.lease and self.lease.holder == client:
            self.lease = None
            return True
        return False

    def is_driver(self, client: str) -> bool:
        self.expire()
        return self.lease is not None and self.lease.holder == client

    def touch(self, client: str) -> bool:
        """Refresh the lease on a drive/ping from the holder. False if the client is not the holder."""
        if self.is_driver(client):
            self.lease.last_seen = self.clock()
            return True
        return False

    def drop_client(self, client: str) -> bool:
        return self.release(client)
