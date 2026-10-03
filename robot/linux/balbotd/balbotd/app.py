# SPDX-License-Identifier: GPL-2.0-only
"""HTTP/WebSocket front end (FastAPI). Authentication, then everything is delegated to the Supervisor."""
from __future__ import annotations

import asyncio
import contextlib
import dataclasses
import json
from typing import Optional

from fastapi import Depends, FastAPI, HTTPException, Request, WebSocket, WebSocketDisconnect, status
from fastapi.responses import JSONResponse

from . import __version__, cfgkeys
from .arbiter import Role, Transport
from .auth import Authenticator, TokenEntry
from .supervisor import ClientViolation, Supervisor


def _token_from(request_headers, query_token: Optional[str]) -> Optional[str]:
    auth = request_headers.get("authorization", "")
    if auth.lower().startswith("bearer "):
        return auth[7:].strip()
    return query_token


def create_app(sup: Supervisor, auth: Authenticator, start_background: bool = True) -> FastAPI:
    @contextlib.asynccontextmanager
    async def lifespan(app: FastAPI):
        if start_background:
            sup.start()
        try:
            yield
        finally:
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

    @app.get("/")
    async def root():
        return {"service": "balbotd", "version": __version__, "api": "/api/v1", "ws": "/ws?token=..."}

    @app.get("/api/v1/state")
    async def get_state(_: TokenEntry = Depends(current(Role.VIEWER))):
        return sup.state_msg()

    @app.get("/api/v1/stats")
    async def get_stats(_: TokenEntry = Depends(current(Role.ADMIN))):
        return {**dataclasses.asdict(sup.stats), "clients": len(sup.clients), "m4f_alive": sup.m4f_alive}

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
        except WebSocketDisconnect:
            pass
        except ClientViolation as e:
            with contextlib.suppress(Exception):
                await ws.close(code=1008, reason=str(e)[:100])
        finally:
            out_task.cancel()
            sup.remove_client(client)

    return app
