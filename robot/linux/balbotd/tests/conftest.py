# SPDX-License-Identifier: GPL-2.0-only
import asyncio
import dataclasses
import json
import os
import sys
import time

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from balbotd import protocol as P  # noqa: E402
from balbotd.arbiter import Role, Transport  # noqa: E402
from balbotd.config import Config  # noqa: E402
from balbotd.fakem4 import FakeConfig, FakeM4F  # noqa: E402
from balbotd.link import MemoryLink  # noqa: E402
from balbotd.supervisor import Supervisor  # noqa: E402


class TestClient_:
    """Drives a Supervisor client directly (no web server)."""

    __test__ = False

    def __init__(self, sup: Supervisor, name: str, role: Role, transport: Transport = Transport.WIFI):
        self.sup = sup
        self.c = sup.add_client(name, role, transport)
        self.t0 = time.monotonic()
        self._seq = 0

    async def send(self, msg: dict) -> None:
        await self.sup.handle(self.c, json.dumps(msg))

    async def send_raw(self, text: str) -> None:
        await self.sup.handle(self.c, text)

    def drain(self) -> list:
        out = []
        while not self.c.out.empty():
            out.append(self.c.out.get_nowait())
        return out

    async def recv(self, type_: str, timeout: float = 1.0, **match):
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                raise AssertionError(f"timeout waiting for {type_} {match}")
            msg = await asyncio.wait_for(self.c.out.get(), left)
            if msg.get("t") == type_ and all(msg.get(k) == v for k, v in match.items()):
                return msg

    async def drive(self, vx: float, wz: float = 0.0, flags: int = 0) -> None:
        self._seq = (self._seq + 1) & 0xFFFF
        await self.send({"t": "drive", "seq": self._seq, "ts": int((time.monotonic() - self.t0) * 1000),
                         "vx": vx, "wz": wz, "flags": flags})

    async def lease(self) -> dict:
        await self.send({"t": "lease", "action": "request"})
        return await self.recv("lease")


async def wait_until(pred, timeout: float = 2.0, step: float = 0.01):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if pred():
            return True
        await asyncio.sleep(step)
    return False


class Rig:
    def __init__(self, cfg: Config, fake_cfg: FakeConfig):
        self.a, self.b = MemoryLink.pair()
        self.fake = FakeM4F(self.b, fake_cfg)
        self.sup = Supervisor(self.a, cfg)

    def client(self, name="drv", role=Role.DRIVER, transport=Transport.WIFI) -> TestClient_:
        return TestClient_(self.sup, name, role, transport)

    async def to_balancing(self, client: TestClient_) -> None:
        assert await wait_until(lambda: self.fake.state == P.ST_STANDBY, 3.0), "fake never reached STANDBY"
        g = await client.lease()
        assert g["granted"], g
        await client.send({"t": "arm"})
        assert await wait_until(lambda: self.fake.state == P.ST_BALANCING, 3.0), "never started balancing"


@pytest.fixture
async def rig():
    fc = FakeConfig(arm_hold_s=0.1, cal_s=0.1, boot_s=0.02)
    r = Rig(Config(), fc)
    r.fake.start()
    r.sup.start()
    yield r
    await r.sup.stop()
    await r.fake.stop()
