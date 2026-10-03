# SPDX-License-Identifier: GPL-2.0-only
"""Real processes, real sockets: fake M4F on a SOCK_SEQPACKET unix socket, balbotd under uvicorn, a WebSocket client.

This exercises the FdLink path (the same code that opens /dev/rpmsgN on the robot) and reconnection."""
import json
import os
import signal
import socket
import subprocess
import sys
import time

import httpx
import pytest
from websockets.sync.client import connect

from balbotd import auth

PKG = os.path.join(os.path.dirname(__file__), "..")


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_for(fn, timeout=10.0, step=0.05):
    end = time.monotonic() + timeout
    last = None
    while time.monotonic() < end:
        try:
            last = fn()
            if last:
                return last
        except Exception as e:  # server not up yet
            last = e
        time.sleep(step)
    raise AssertionError(f"timeout, last={last!r}")


@pytest.fixture
def stack(tmp_path):
    token, h = auth.new_token()
    conf = tmp_path / "balbotd.toml"
    conf.write_text(f'[[tokens]]\nname = "t"\nrole = "admin"\nsha256 = "{h}"\n')
    sock = str(tmp_path / "m4.sock")
    port = free_port()
    procs = []

    def start_fake():
        p = subprocess.Popen([sys.executable, "-m", "balbotd.fakem4", "--socket", sock], cwd=PKG)
        procs.append(p)
        wait_for(lambda: os.path.exists(sock), 5)
        return p

    fake = start_fake()
    daemon = subprocess.Popen([sys.executable, "-m", "balbotd", "--config", str(conf), "--m4-socket", sock,
                               "--host", "127.0.0.1", "--port", str(port)], cwd=PKG,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    procs.append(daemon)
    base = f"http://127.0.0.1:{port}"
    h_ = {"Authorization": "Bearer " + token}
    wait_for(lambda: httpx.get(base + "/").status_code == 200, 10)

    class S:
        pass

    s = S()
    s.base, s.headers, s.token, s.port, s.sock = base, h_, token, port, sock
    s.fake = fake
    s.start_fake = start_fake
    s.state = lambda: httpx.get(base + "/api/v1/state", headers=h_).json()
    yield s
    for p in procs:
        if p.poll() is None:
            p.send_signal(signal.SIGTERM)
    for p in procs:
        try:
            p.wait(5)
        except subprocess.TimeoutExpired:
            p.kill()


def recv_until(ws, pred, timeout=5.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        m = json.loads(ws.recv(timeout=max(0.05, end - time.monotonic())))
        if pred(m):
            return m
    raise AssertionError("timeout")


def test_full_session_over_real_sockets_and_reconnect(stack):
    st = wait_for(lambda: (lambda s: s if s["state"] == "STANDBY" and s["m4f_alive"] else None)(stack.state()), 10)
    assert st["driver"] is None

    with connect(f"ws://127.0.0.1:{stack.port}/ws?token={stack.token}") as ws:
        assert json.loads(ws.recv())["t"] == "hello"
        ws.send(json.dumps({"t": "lease", "action": "request"}))
        assert recv_until(ws, lambda m: m["t"] == "lease")["granted"]
        ws.send(json.dumps({"t": "arm"}))
        end = time.monotonic() + 6
        balancing = False
        while time.monotonic() < end and not balancing:  # keep the lease alive while the 1 s arm window runs
            ws.send(json.dumps({"t": "ping", "ts": int(time.monotonic() * 1000)}))
            m = recv_until(ws, lambda m: m["t"] in ("state", "pong"))
            balancing = m["t"] == "state" and m["state"] == "BALANCING"
            time.sleep(0.1)
        assert balancing
        ws.send(json.dumps({"t": "sub", "rate": 30}))
        t0 = time.monotonic()
        seq = 0
        best = 0.0
        while time.monotonic() - t0 < 1.5:
            seq += 1
            ws.send(json.dumps({"t": "drive", "seq": seq, "vx": 1.0, "wz": 0.0}))
            time.sleep(0.02)
        for _ in range(400):
            m = recv_until(ws, lambda m: m["t"] == "tlm")
            best = max(best, m["v"])
            if best > 0.2:
                break
        assert best > 0.2

        # the M4F process dies: balbotd must notice and keep serving
        stack.fake.kill()
        stack.fake.wait()
        st = wait_for(lambda: (lambda s: s if s["m4f_alive"] is False else None)(stack.state()), 5)
        assert "M4F_DEAD" in st["fault_names"]
        # it comes back (firmware restarted): balbotd reconnects by itself
        stack.start_fake()
        st = wait_for(lambda: (lambda s: s if s["m4f_alive"] and s["state"] in ("BOOT", "CALIBRATING", "STANDBY") else None)(
            stack.state()), 10)
        stats = httpx.get(stack.base + "/api/v1/stats", headers=stack.headers).json()
        assert stats["reconnects"] >= 2 and stats["crc_errors"] == 0
