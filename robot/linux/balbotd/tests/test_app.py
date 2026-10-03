# SPDX-License-Identifier: GPL-2.0-only
"""HTTP and WebSocket front end, with the fake M4F running inside the app (`balbotd --fake-m4` setup)."""
import json
import time

import pytest
from fastapi.testclient import TestClient
from starlette.websockets import WebSocketDisconnect

from balbotd import auth
from balbotd.__main__ import build_app
from balbotd.arbiter import Role
from balbotd.config import Config
from balbotd.auth import TokenEntry


@pytest.fixture
def tokens():
    out = {}
    entries = []
    for role in ("viewer", "driver", "admin"):
        t, h = auth.new_token()
        out[role] = t
        entries.append(TokenEntry(role, Role[role.upper()], h))
    return out, entries


@pytest.fixture
def client(tokens):
    toks, entries = tokens
    cfg = Config(tokens=entries)
    app = build_app(cfg, fake=True)
    with TestClient(app) as c:
        c.tokens = toks
        yield c


def hdr(c, role):
    return {"Authorization": "Bearer " + c.tokens[role]}


def wait_for(fn, timeout=3.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.02)
    raise AssertionError("timeout")


def ws_wait_state(ws, state, timeout=5.0):
    """Keep the lease alive with pings (a real client does) until the robot reports `state`."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        ws.send_text(json.dumps({"t": "ping", "ts": int(time.monotonic() * 1000)}))
        while True:
            m = json.loads(ws.receive_text())
            if m.get("t") == "state" and m["state"] == state:
                return m
            if m.get("t") == "pong":
                break
        time.sleep(0.1)
    raise AssertionError("never reached " + state)


def ws_recv(ws, type_, **match):
    for _ in range(500):
        m = json.loads(ws.receive_text())
        if m.get("t") == type_ and all(m.get(k) == v for k, v in match.items()):
            return m
    raise AssertionError(f"no {type_} {match}")


def test_root_is_open_but_the_api_is_not(client):
    assert client.get("/").json()["service"] == "balbotd"
    assert client.get("/api/v1/state").status_code == 401
    assert client.get("/api/v1/state", headers={"Authorization": "Bearer nope"}).status_code == 401
    assert client.get("/api/v1/state?token=" + client.tokens["viewer"]).status_code == 200


def test_roles_gate_the_rest_endpoints(client):
    assert client.get("/api/v1/state", headers=hdr(client, "viewer")).json()["t"] == "state"
    for ep in ("/api/v1/stats", "/api/v1/config"):
        assert client.get(ep, headers=hdr(client, "driver")).status_code == 403
        assert client.get(ep, headers=hdr(client, "admin")).status_code == 200
    assert client.put("/api/v1/config", json={"pid.angle.kp": 50}, headers=hdr(client, "driver")).status_code == 403


def test_brute_force_is_throttled_with_429(client):
    for _ in range(5):
        assert client.get("/api/v1/state", headers={"Authorization": "Bearer wrong"}).status_code == 401
    assert client.get("/api/v1/state", headers={"Authorization": "Bearer wrong"}).status_code == 429
    assert client.get("/api/v1/state", headers=hdr(client, "admin")).status_code == 429  # blocked per source


def test_state_reflects_the_fake_robot(client):
    st = wait_for(lambda: (lambda s: s if s["state"] == "STANDBY" else None)(
        client.get("/api/v1/state", headers=hdr(client, "viewer")).json()))
    assert st["m4f_alive"] is True and st["driver"] is None


def test_config_get_put_round_trip(client):
    cfg = client.get("/api/v1/config", headers=hdr(client, "admin")).json()
    assert cfg["pid.angle.kp"]["value"] == 60.0 and cfg["limit.v_max"]["safety"] is True
    wait_for(lambda: client.get("/api/v1/state", headers=hdr(client, "viewer")).json()["state"] == "STANDBY")
    r = client.put("/api/v1/config", json={"pid.angle.kp": 51.5, "pid.angle.kd": 6.0}, headers=hdr(client, "admin"))
    assert r.status_code == 200 and r.json()["pid.angle.kp"]["status"] == "ok"
    assert client.get("/api/v1/config", headers=hdr(client, "admin")).json()["pid.angle.kp"]["value"] == 51.5
    r = client.put("/api/v1/config", json={"pid.angle.kp": 1e9}, headers=hdr(client, "admin"))
    assert r.status_code == 409 and r.json()["pid.angle.kp"]["code"] == "OUT_OF_RANGE"
    r = client.put("/api/v1/config", json={"nonsense": 1}, headers=hdr(client, "admin"))
    assert r.status_code == 409 and r.json()["nonsense"]["code"] == "UNKNOWN_KEY"


def test_websocket_requires_a_valid_token(client):
    for url in ("/ws", "/ws?token=bad"):
        with pytest.raises(WebSocketDisconnect) as e:
            with client.websocket_connect(url):
                pass
        assert e.value.code == 1008


def test_websocket_drive_session_end_to_end(client):
    with client.websocket_connect("/ws?token=" + client.tokens["driver"]) as ws:
        hello = json.loads(ws.receive_text())
        assert hello["t"] == "hello" and hello["role"] == "driver"
        wait_for(lambda: client.get("/api/v1/state", headers=hdr(client, "viewer")).json()["state"] == "STANDBY")
        ws.send_text(json.dumps({"t": "lease", "action": "request"}))
        assert ws_recv(ws, "lease")["granted"] is True
        ws.send_text(json.dumps({"t": "arm"}))
        ws_wait_state(ws, "BALANCING")  # fake arm hold is 1 s; pings keep the 1 s lease alive meanwhile
        ws.send_text(json.dumps({"t": "sub", "rate": 20}))
        t0 = time.monotonic()
        seq = 0
        while time.monotonic() - t0 < 1.5:  # 50 Hz full stick; telemetry queues up meanwhile
            seq += 1
            ws.send_text(json.dumps({"t": "drive", "seq": seq, "vx": 1.0, "wz": 0.0}))
            time.sleep(0.02)
        best = 0.0
        for _ in range(300):  # the queue is ordered: the accelerating samples are in there
            m = ws_recv(ws, "tlm")
            best = max(best, m["v"])
            if best > 0.2:
                break
        assert best > 0.2
        ws.send_text(json.dumps({"t": "estop"}))
        st = ws_recv(ws, "state", state="FAULT")
        assert "ESTOP" in st["fault_names"]


def test_viewer_websocket_cannot_drive_but_can_estop(client):
    with client.websocket_connect("/ws?token=" + client.tokens["viewer"]) as ws:
        ws.receive_text()
        ws.send_text(json.dumps({"t": "lease", "action": "request"}))
        assert ws_recv(ws, "lease")["reason"] == "role"
        ws.send_text(json.dumps({"t": "drive", "seq": 1, "vx": 1, "wz": 0}))
        assert ws_recv(ws, "err")["code"] == "NOT_DRIVER"
        ws.send_text(json.dumps({"t": "estop"}))
        ws_recv(ws, "state", state="FAULT")


def test_websocket_limits_close_the_connection(client):
    with client.websocket_connect("/ws?token=" + client.tokens["viewer"]) as ws:
        ws.receive_text()
        ws.send_text("x" * 5000)
        with pytest.raises(WebSocketDisconnect) as e:
            for _ in range(10):
                ws.receive_text()
        assert e.value.code == 1008
    stats = client.get("/api/v1/stats", headers=hdr(client, "admin")).json()
    assert stats["violations"] >= 1
    assert stats["clients"] == 0  # the connection was cleaned up


def test_disconnect_releases_the_lease(client):
    with client.websocket_connect("/ws?token=" + client.tokens["driver"]) as ws:
        ws.receive_text()
        ws.send_text(json.dumps({"t": "lease", "action": "request"}))
        ws_recv(ws, "lease")
        assert client.get("/api/v1/state", headers=hdr(client, "viewer")).json()["driver"] == "driver"
    wait_for(lambda: client.get("/api/v1/state", headers=hdr(client, "viewer")).json()["driver"] is None)
