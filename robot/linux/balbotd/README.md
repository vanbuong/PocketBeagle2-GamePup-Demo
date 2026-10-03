# balbotd

Linux-side supervisor of the balance robot (docs: [01 architecture](../../../docs/balance-robot/01-system-architecture.md) 1.5-1.7,
[03 remote control](../../../docs/balance-robot/03-remote-control.md)). It is **not** in the balance loop: it relays
intent to the M4F, enforces who may drive, and fails safe when a client or the M4F goes quiet.

```
browser / app --WebSocket--> balbotd --rpmsg frames--> M4F (or FakeM4F)
```

| Module | Role |
|---|---|
| `protocol.py` | rpmsg frames and payloads; byte-compatible with `robot/core` (tested against `libbalbot.so`) |
| `supervisor.py` | message handling, lease, 50 Hz CMD_DRIVE, 10 Hz heartbeat, failsafe, telemetry fan-out, config round trips, M4F liveness, reconnect |
| `arbiter.py` | control lease: one driver, 1 s expiry, transport priority (physical > WiFi > BLE), epochs 1..255 |
| `shaping.py` | deadband + expo, v_max/w_max scaling, boost, int16-safe |
| `app.py` | FastAPI: bearer-token auth with roles, `/api/v1/{state,stats,config,video/*}`, `/ws`, the UI at `/ui/` with a strict CSP |
| `video.py` | MJPEG hub: camera JPEGs forwarded without decoding, newest-frame-only per viewer, source stops when nobody watches; ffmpeg or command sources, built-in test pattern |
| `web/` | the browser control page: plain ES modules, no build step (see below) |
| `link.py` | `MemoryLink` (tests), `FdLink` (`/dev/rpmsgN` or a SOCK_SEQPACKET socket); send never blocks, drops are counted |
| `fakem4.py` | behavioural stand-in for the M4F firmware, with fault injection (hang, tip over, low battery, lossy link) |

## Run it without hardware

```sh
pip install -e '.[test]'
python -m balbotd --new-token admin           # prints the token once and the [[tokens]] entry for the config
python -m balbotd --config balbotd.toml --fake-m4 --port 8080
curl -H "Authorization: Bearer <token>" localhost:8080/api/v1/state
```

Browsers cannot set headers on a WebSocket, so `/ws?token=<token>` is accepted as well. Two-process variant, which exercises the same
code that opens `/dev/rpmsgN`:

```sh
python -m balbotd.fakem4 --socket /tmp/fakem4.sock &
python -m balbotd --config balbotd.toml --m4-socket /tmp/fakem4.sock
```

On the robot: `--rpmsg /dev/rpmsg0` (see `systemd/balbotd.service`, unverified on hardware).

## WebSocket messages (v1)

Client to robot: `lease {action: request|release}`, `drive {seq, vx, wz, flags, ts?}` (flags bit1 = boost), `arm`, `disarm`, `estop`,
`reset` (lease or admin), `ping {ts}`, `sub {rate}`, `cfg_get {key}`, `cfg_set {key, value}` (admin), `cfg_save` (admin), `cal {kind: gyro|level}` (admin).
Robot to client: `hello`, `state`, `tlm`, `lease`, `pong`, `cfg`, `err {code}`.

Behaviour worth knowing:

- The lease expires **1 s after the last drive or ping** from the holder, so a client must keep talking while it waits (e.g. for the arm window).
- No fresh `drive` for 250 ms sends zero velocity; the robot keeps balancing. A new lease never starts from an old command.
- `estop` is accepted from any authenticated client (any role) and sent three times; `disarm` likewise.
- Limits: 4 KiB per message, 200 messages/s, bounded outgoing queue; violators are disconnected. 5 failed token attempts per minute block a source.
- Drive frames whose timestamp is more than 250 ms older than the freshest one seen from that client are dropped.

## Browser control page

Open `http://<robot>:8080/` (redirects to `/ui/`). Enter an access token (or open `/ui/#token=...`: the fragment is read once and removed from the
address bar). The token is kept in `sessionStorage` for the tab only.

| Control | What it does |
|---|---|
| Take control / Release control | requests or gives up the lease; a reconnect never re-takes it |
| Left stick (up/down), right stick (left/right) | speed and turn; dragging **left turns left**; both sticks work at once on touch screens |
| W/S or arrows, A/D, Shift | keyboard drive, turn, boost; Esc releases control |
| Gamepad (standard mapping) | left stick Y speed, right stick X turn, B emergency stop, X take control |
| Arm / Disarm / Reset fault | enabled only when the robot state allows them; reset needs the lease or an admin token |
| **E-STOP** (also Space bar, gamepad B) | always enabled while connected, works for viewers |

Safety rules in the page: input that is not refreshed for 500 ms counts as zero; losing focus or hiding the tab zeroes the sticks, and hiding the tab
also releases the lease; the video is `<img src=/api/v1/video/mjpeg?token=...>` (an `<img>` cannot send headers, so the token is in the query string:
fine on a private LAN or AP, avoid sharing URLs that contain it).

Video: set `[video]` in the config. `mode = "camera"` runs `ffmpeg ... -c:v copy` on `/dev/video0` (MJPG passthrough, nothing is decoded or
re-encoded); `mode = "command"` takes any command that writes concatenated JPEGs to stdout; `--fake-m4` shows a moving test pattern. The page's
settings dialog picks 30/15/5 fps; the server thins the stream, the camera still runs at its native rate.

## Tests

```sh
pip install -e '.[test]'
cmake -S ../../core -B ../../core/build-shared -G Ninja -DBUILD_SHARED=ON && cmake --build ../../core/build-shared --target balbot
python -m pytest -q          # ~50 s; C cross-checks run when libbalbot.so is built (or set BALBOT_LIB)
```

`tests/*.py` (pytest) plus `tests/js/*.test.mjs` (`node --test`, Node 22). `test_browser.py` drives the page in a real Chromium (Playwright; set
`BALBOT_CHROMIUM` or run `playwright install chromium`) with the content-security policy enforced: login, drive with mouse, touch and keyboard, E-stop and
reset, viewer limits, reconnect, video, layout at three screen sizes, text contrast in light and dark. Current numbers: 153 Python tests (92 % line coverage; subprocess entry points are not measured), 24 Node tests; the 21 browser tests passed 3 runs in a row. `tests/test_process.py` starts the fake M4F and balbotd as
separate processes, drives over a real WebSocket, kills the fake and checks that balbotd reports `M4F_DEAD` and reconnects.

## Not done yet

BLE bridge, video, OTA, blackbox logging, web UI, TLS; the lease and failsafe constants are untested on real WiFi; the fake M4F is not the
firmware, so a green run here says nothing about the real M4F's timing.
