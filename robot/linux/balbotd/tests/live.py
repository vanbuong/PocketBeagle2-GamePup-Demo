# SPDX-License-Identifier: GPL-2.0-only
"""Helpers that run the real app (fake M4F, test-pattern camera) under uvicorn in a thread."""
import time

from balbotd import auth
from balbotd.__main__ import build_app
from balbotd.arbiter import Role
from balbotd.auth import TokenEntry
from balbotd.config import Config, VideoConfig


def make_client(video_mode="pattern", tokens=True):
    toks, entries = {}, []
    for role in ("viewer", "driver", "admin"):
        t, h = auth.new_token()
        toks[role] = t
        entries.append(TokenEntry(role, Role[role.upper()], h))
    cfg = Config(tokens=entries, video=VideoConfig(mode=video_mode, idle_stop_s=0.1))
    return build_app(cfg, fake=True), toks




class Live:
    """The real app under uvicorn in a thread: TestClient buffers whole responses, so it cannot read an endless MJPEG stream."""

    def __init__(self, app, toks):
        import socket
        import threading
        import uvicorn
        with socket.socket() as sk:
            sk.bind(("127.0.0.1", 0))
            self.port = sk.getsockname()[1]
        self.server = uvicorn.Server(uvicorn.Config(app, host="127.0.0.1", port=self.port, log_level="warning"))
        self.thread = threading.Thread(target=self.server.run, daemon=True)
        self.thread.start()
        end = time.monotonic() + 10
        while not self.server.started and time.monotonic() < end:
            time.sleep(0.02)
        assert self.server.started
        self.base = f"http://127.0.0.1:{self.port}"
        self.tokens = toks

    def stop(self):
        self.server.should_exit = True
        self.thread.join(10)


