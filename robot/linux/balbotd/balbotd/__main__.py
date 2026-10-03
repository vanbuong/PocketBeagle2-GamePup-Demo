# SPDX-License-Identifier: GPL-2.0-only
"""balbotd entry point.

  python -m balbotd --config balbotd.toml --rpmsg /dev/rpmsg0         # on the robot
  python -m balbotd --config balbotd.toml --fake-m4                    # development, no hardware
  python -m balbotd --config balbotd.toml --m4-socket /run/fakem4.sock # against `python -m balbotd.fakem4 --socket ...`
  python -m balbotd --new-token driver                                 # print a token and its config entry
"""
from __future__ import annotations

import argparse
import contextlib
import sys
import time

from . import auth as authmod
from .app import create_app
from .arbiter import Role
from .config import Config, load
from .fakem4 import FakeConfig, FakeM4F, serve_unix
from .video import CommandSource, PatternSource, VideoHub, camera_command
from .link import FdLink, MemoryLink
from .supervisor import Supervisor


def build_app(cfg: Config, rpmsg: str | None = None, m4_socket: str | None = None, fake: bool = False):
    clock = time.monotonic
    auth = authmod.Authenticator(cfg.tokens, clock)
    fake_m4 = None
    factory = None
    link = None
    if fake:
        a, b = MemoryLink.pair()
        link = a
        fake_m4 = FakeM4F(b, FakeConfig())
    elif rpmsg:
        async def factory():
            return FdLink.open_rpmsg(rpmsg)
    elif m4_socket:
        async def factory():
            return FdLink.connect_seqpacket(m4_socket)
    else:
        raise SystemExit("choose one of --rpmsg PATH, --m4-socket PATH or --fake-m4")
    sup = Supervisor(link, cfg, clock, link_factory=factory)
    mode = cfg.video.mode if cfg.video.mode != "auto" else ("pattern" if fake else "off")
    hub = None
    if mode == "pattern":
        hub = VideoHub(PatternSource(), cfg.video.idle_stop_s)
    elif mode == "camera":
        hub = VideoHub(CommandSource(camera_command(cfg.video.device, cfg.video.size, cfg.video.fps)), cfg.video.idle_stop_s)
    elif mode == "command":
        hub = VideoHub(CommandSource(list(cfg.video.command)), cfg.video.idle_stop_s)
    app = create_app(sup, auth, start_background=False, video=hub)
    app.state.fake_m4 = fake_m4  # None unless --fake-m4: lets tests poke the fake robot

    @contextlib.asynccontextmanager
    async def lifespan(a):
        if fake_m4:
            fake_m4.start()
        sup.start()
        try:
            yield
        finally:
            await sup.stop()
            if fake_m4:
                await fake_m4.stop()

    app.router.lifespan_context = lifespan
    return app


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="balbotd")
    ap.add_argument("--config")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8080)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--rpmsg")
    g.add_argument("--m4-socket")
    g.add_argument("--fake-m4", action="store_true")
    ap.add_argument("--new-token", choices=["viewer", "driver", "admin"])
    args = ap.parse_args(argv)

    if args.new_token:
        token, h = authmod.new_token()
        print(f"token (keep secret, shown once): {token}")
        print("add to balbotd.toml:\n[[tokens]]\nname = \"%s\"\nrole = \"%s\"\nsha256 = \"%s\"" % (args.new_token, args.new_token, h))
        return 0
    cfg = load(args.config) if args.config else Config()
    if not cfg.tokens:
        print("warning: no tokens configured; every request will be rejected", file=sys.stderr)
    import uvicorn
    uvicorn.run(build_app(cfg, args.rpmsg, args.m4_socket, args.fake_m4), host=args.host, port=args.port,
                log_level="info", ws_max_size=8192)
    return 0


if __name__ == "__main__":
    sys.exit(main())
