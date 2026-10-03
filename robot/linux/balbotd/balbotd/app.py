# SPDX-License-Identifier: GPL-2.0-only
"""HTTP/WebSocket front end (FastAPI). Authentication, then everything is delegated to the Supervisor."""
from __future__ import annotations

import asyncio
import contextlib
import dataclasses
import json
import os
import time
from typing import Optional

from fastapi import Depends, FastAPI, HTTPException, Request, WebSocket, WebSocketDisconnect, status
from fastapi.responses import JSONResponse, RedirectResponse, Response, StreamingResponse
from fastapi.staticfiles import StaticFiles

from . import __version__, cfgkeys
from .arbiter import Role, Transport
from .auth import Authenticator, TokenEntry
from .supervisor import ClientViolation, Supervisor
from .video import VideoHub

WEB_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web")
# The UI is plain files with no inline script or style. Images may come from this origin or blobs only.
CSP = ("default-src 'self'; img-src 'self' data: blob:; connect-src 'self' ws: wss:; style-src 'self'; "
       "script-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'")


def _token_from(request_headers, query_token: Optional[str]) -> Optional[str]:
    auth = request_headers.get("authorization", "")
    if auth.lower().startswith("bearer "):
        return auth[7:].strip()
    return query_token


def create_app(sup: Supervisor, auth: Authenticator, start_background: bool = True,
               video: Optional[VideoHub] = None) -> FastAPI:
    @contextlib.asynccontextmanager
    async def lifespan(app: FastAPI):
        if start_background:
            sup.start()
        try:
            yield
        finally:
            if video:
                await video.stop()
            if start_background:
                await sup.stop()

    app = FastAPI(title="balbotd", version=__version__, lifespan=lifespan)
    app.state.supervisor = sup

    def current(min_role: Role):
        def dep(request: Request, token: Optional[str] = None) -> TokenEntry:
            src = request.client.host if request.client else "?"
            if auth.blocked(src):
                raise HTTPException(status.HTTP_429_TOO_MANY_REQUESTS, "too many failed attempts")
            entry = auth.verify(_token_from(request.headers, token), src)
            if entry is None:
                raise HTTPException(status.HTTP_401_UNAUTHORIZED, "invalid or missing token",
                                    headers={"WWW-Authenticate": "Bearer"})
            if entry.role < min_role:
                raise HTTPException(status.HTTP_403_FORBIDDEN, "insufficient role")
            return entry
        return dep

    @app.middleware("http")
    async def ui_headers(request: Request, call_next):
        resp = await call_next(request)
        if request.url.path.startswith("/ui"):
            resp.headers["Content-Security-Policy"] = CSP
            resp.headers["X-Content-Type-Options"] = "nosniff"
            resp.headers["Referrer-Policy"] = "no-referrer"
            resp.headers["Cache-Control"] = "no-cache"
        return resp

    @app.get("/")
    async def root(request: Request):
        if "text/html" in request.headers.get("accept", ""):
            return RedirectResponse("/ui/")
        return {"service": "balbotd", "version": __version__, "api": "/api/v1", "ws": "/ws?token=...", "ui": "/ui/"}

    @app.get("/api/v1/state")
    async def get_state(_: TokenEntry = Depends(current(Role.VIEWER))):
        return sup.state_msg()

    @app.get("/api/v1/stats")
    async def get_stats(_: TokenEntry = Depends(current(Role.ADMIN))):
        out = {**dataclasses.asdict(sup.stats), "clients": len(sup.clients), "m4f_alive": sup.m4f_alive}
        if video:
            out["video"] = {**dataclasses.asdict(video.stats), "source": type(video.source).__name__}
        return out

    @app.get("/api/v1/video/info")
    async def video_info(_: TokenEntry = Depends(current(Role.VIEWER))):
        return {"enabled": video is not None, "mjpeg": "/api/v1/video/mjpeg", "snapshot": "/api/v1/video/snapshot"}

    @app.get("/api/v1/video/snapshot")
    async def video_snapshot(_: TokenEntry = Depends(current(Role.VIEWER))):
        if video is None:
            raise HTTPException(status.HTTP_404_NOT_FOUND, "video is disabled")
        f = await video.snapshot()
        if f is None:
            raise HTTPException(status.HTTP_503_SERVICE_UNAVAILABLE, "no frame available")
        return Response(f.data, media_type=f.mime, headers={"Cache-Control": "no-store"})

    @app.get("/api/v1/video/mjpeg")
    async def video_mjpeg(fps: float = 0.0, _: TokenEntry = Depends(current(Role.VIEWER))):
        """multipart/x-mixed-replace stream. `fps` (1..30) thins the stream to save bandwidth; 0 = every frame."""
        if video is None:
            raise HTTPException(status.HTTP_404_NOT_FOUND, "video is disabled")
        interval = 1.0 / min(30.0, max(1.0, fps)) if fps > 0 else 0.0

        async def gen():
            sub = video.subscribe()
            last_sent = 0.0
            idle = 0.0
            try:
                while True:
                    f = await sub.next(1.0)
                    if f is None:
                        idle += 1.0
                        if idle >= 20.0:  # a dead source: end the response, the client reconnects
                            return
                        continue
                    idle = 0.0
                    now = time.monotonic()
                    if interval and now - last_sent < interval:
                        continue
                    last_sent = now
                    yield (b"--frame\r\nContent-Type: " + f.mime.encode() + b"\r\nContent-Length: "
                           + str(len(f.data)).encode() + b"\r\n\r\n" + f.data + b"\r\n")
            finally:
                sub.close()

        return StreamingResponse(gen(), media_type="multipart/x-mixed-replace; boundary=frame",
                                 headers={"Cache-Control": "no-store"})


    @app.get("/api/v1/config")
    async def get_config(_: TokenEntry = Depends(current(Role.ADMIN))):
        return {k.name: {"id": k.id, "value": sup.cfg_cache.get(k.name, k.default), "min": k.lo, "max": k.hi,
                         "safety": k.safety} for k in cfgkeys.KEYS}

    @app.put("/api/v1/config")
    async def put_config(body: dict, entry: TokenEntry = Depends(current(Role.ADMIN))):
        """Apply {key: value, ...} through a temporary admin client; replies are collected per key."""
        c = sup.add_client("rest:" + entry.name, Role.ADMIN, Transport.WIFI)
        results = {}
        try:
            while not c.out.empty():
                c.out.get_nowait()  # discard hello/state
            for name, value in body.items():
                await sup.handle(c, json.dumps({"t": "cfg_set", "key": name, "value": value}))
                msg = c.out.get_nowait()
                results[name] = msg
        except ClientViolation as e:
            raise HTTPException(status.HTTP_429_TOO_MANY_REQUESTS, str(e))
        finally:
            sup.remove_client(c)
        ok = all(m.get("t") == "cfg" and m.get("status") == "ok" for m in results.values())
        return JSONResponse(results, status_code=200 if ok else 409)

    @app.websocket("/ws")
    async def ws(ws: WebSocket, token: Optional[str] = None):
        src = ws.client.host if ws.client else "?"
        if auth.blocked(src):
            await ws.close(code=1008, reason="too many failed attempts")
            return
        entry = auth.verify(_token_from(ws.headers, token), src)
        if entry is None:
            await ws.close(code=1008, reason="invalid or missing token")
            return
        await ws.accept()
        client = sup.add_client(entry.name, entry.role, Transport.WIFI)

        async def pump_out():
            while True:
                msg = await client.out.get()
                if msg is None:
                    return
                await ws.send_text(json.dumps(msg, separators=(",", ":")))

        out_task = asyncio.create_task(pump_out())
        try:
            while True:
                text = await ws.receive_text()
                await sup.handle(client, text)
                if client.closed:
                    break
        except (WebSocketDisconnect, RuntimeError):  # RuntimeError: peer vanished between accept and the first read
            pass
        except ClientViolation as e:
            with contextlib.suppress(Exception):
                await ws.close(code=1008, reason=str(e)[:100])
        finally:
            out_task.cancel()
            sup.remove_client(client)

    if os.path.isdir(WEB_DIR):
        app.mount("/ui", StaticFiles(directory=WEB_DIR, html=True), name="ui")
    return app
