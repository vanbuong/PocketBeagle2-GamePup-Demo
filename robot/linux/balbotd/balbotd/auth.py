# SPDX-License-Identifier: GPL-2.0-only
"""Bearer-token authentication with roles, plus failed-attempt throttling."""
from __future__ import annotations

import hashlib
import hmac
import secrets
from dataclasses import dataclass
from typing import Callable, Optional

from .arbiter import Role


def hash_token(token: str) -> str:
    return hashlib.sha256(token.encode()).hexdigest()


def new_token() -> tuple[str, str]:
    """(token, sha256 hex). Only the hash is stored in the configuration file."""
    t = secrets.token_urlsafe(16)  # 128 bits
    return t, hash_token(t)


@dataclass(frozen=True)
class TokenEntry:
    name: str
    role: Role
    sha256: str


class Authenticator:
    def __init__(self, entries: list[TokenEntry], clock: Callable[[], float],
                 max_failures: int = 5, window_s: float = 60.0):
        self.entries = entries
        self.clock = clock
        self.max_failures = max_failures
        self.window_s = window_s
        self._fails: dict[str, list[float]] = {}

    def blocked(self, source: str) -> bool:
        now = self.clock()
        fails = [t for t in self._fails.get(source, []) if now - t < self.window_s]
        self._fails[source] = fails
        return len(fails) >= self.max_failures

    def verify(self, token: Optional[str], source: str = "?") -> Optional[TokenEntry]:
        """Matching entry, or None. Constant-time compare against every entry; failures are throttled per source."""
        if self.blocked(source):
            return None
        h = hash_token(token or "")
        found = None
        for e in self.entries:
            if hmac.compare_digest(e.sha256, h):
                found = e
        if found is None:
            self._fails.setdefault(source, []).append(self.clock())
        return found
