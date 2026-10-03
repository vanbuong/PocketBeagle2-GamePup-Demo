# 3. Remote control over WiFi and Bluetooth

## 3.1 Channels

| Channel | Use | Rate | Payload |
|---|---|---|---|
| WebSocket `/ws` over WiFi | primary: control + telemetry, browser and RN app | 50 Hz up, 10–50 Hz down | JSON (v1) → binary (MessagePack/CBOR) later |
| BLE GATT | fallback control, WiFi provisioning, pairing, no-video driving | 20–50 Hz up, 5–10 Hz down | compact binary, ≤ 20 B frames |
| REST `/api/v1` | configuration, calibration, OTA, state | on demand | JSON |
| Physical (E-stop button) | safety | — | hardware |

Bluetooth **Classic (SPP)** is not used: iOS does not allow it for generic apps. BLE works
on iOS and Android and fits the control payload.

## 3.2 Control semantics

Clients send a normalised *intent*, never motor values:

- `vx` ∈ [−1, 1] → forward velocity (scaled by `v_max`, default 0.6 m/s *(initial)*)
- `wz` ∈ [−1, 1] → yaw rate (scaled by `w_max`, default 2.5 rad/s *(initial)*)
- `mode` flags: `balance_on`, `boost` (raises limits), `hold_position`
- `seq` (u16 wrapping), `ts` (client monotonic ms)

`balbotd` converts to `v_cmd` mm/s, `w_cmd` mrad/s, applies deadband (5 %), expo curve
(`y = 0.6·x³ + 0.4·x` *(initial)*), slew limiting and limits, then forwards every 20 ms as
`CMD_DRIVE`. The M4F applies a second, independent clamp and acceleration limit (so a buggy
or malicious `balbotd` cannot command unsafe lean): the firmware limits are authoritative.

## 3.3 WebSocket protocol (v1)

Connect to `wss://balbot.local/ws?token=<jwt-or-pin-token>` (or `ws://` on the AP, doc 7 Q4).
First message from server: `hello`. Messages are JSON objects with `t` (type):

Client → robot:

```json
{"t":"drive","seq":1203,"ts":1893344,"vx":0.35,"wz":-0.10,"flags":1}
{"t":"arm"}   {"t":"disarm"}   {"t":"estop"}
{"t":"lease","action":"request"}            // become the driver
{"t":"ping","ts":1893344}
{"t":"cfg_set","key":"pid.angle.kp","value":18.5}   // admin only, rejected while BALANCING for unsafe keys
```

Robot → client:

```json
{"t":"hello","proto":1,"fw":"0.1.0","role":"driver|viewer","epoch":7}
{"t":"state","state":"BALANCING","faults":0,"vbat":7.91,"rssi":-52}
{"t":"tlm","ts":1893350,"pitch":0.8,"pitch_rate":-2.1,"yaw_rate":0.0,"v":0.31,"uL":0.12,"uR":0.11,"cpu":0.04}
{"t":"pong","ts":1893344,"srv":1893351}
{"t":"err","code":"NOT_DRIVER","msg":"..."}
```

Implemented in `robot/linux/balbotd` (see its README for the message list). Notes from implementation:

- The lease expires 1 s after the last `drive` **or `ping`**, so a client must keep sending while it waits, for example during the arm window.
- `drive.flags`: bit1 = boost. `estop` and `disarm` are sent to the M4F three times (15 ms apart) because they are idempotent and one lost frame must not matter.

Rules:

- Messages > 4 KB or > 200 msg/s from a client → connection dropped.
- `drive` older than 250 ms by `ts` (after offset estimation via ping/pong) is discarded.
- `seq` must be monotonic modulo 2¹⁶; duplicates dropped, large gaps logged.
- Telemetry is decimated per client according to a `subscribe` message (`{"t":"sub","rate":20}`).

## 3.4 BLE GATT service

Advertising name `balbot-XXXX`, service UUID `7b0b0001-ba1b-4c5d-9e00-ba1bba1bba1b`
*(placeholder; generate a real random UUID at implementation)*.

| Characteristic | UUID suffix | Props | Format |
|---|---|---|---|
| Control | `…0002` | write w/o response | 8 B: `u16 seq`, `i16 vx` (×1/1000), `i16 wz` (×1/1000), `u8 flags`, `u8 crc8` |
| Command | `…0003` | write | `u8 op` (arm 1, disarm 2, estop 3, lease 4), `u8 arg` |
| Telemetry | `…0004` | notify | 18 B: `u8 state`, `i16 pitch`(0.01°), `i16 v`(mm/s), `u16 vbat`(mV), `u8 faults`, `i8 rssi`, … |
| Status | `…0005` | read, notify | state + fw version |
| WiFi provisioning | `…0006` | write (encrypted link only) | SSID/PSK TLV, reply on Status |
| Config | `…0007` | write/read | key/value (admin, encrypted link) |

- Frames fit the default 20-byte ATT payload so no MTU negotiation is required; if MTU is
  larger, the same frames are used. Connection interval requested 15–30 ms, latency 0,
  supervision timeout 2 s.
- Security: LE Secure Connections with *passkey display* — the robot shows a 6-digit PIN on
  the ILI9341/OLED (the GamePup display from this repo) and the phone user enters it. Bonded
  devices persist. Provisioning and Config characteristics require an encrypted, bonded link.
- Implementation: BlueZ via D-Bus (`dbus-fast` GATT application) inside `balbot-ble.service`,
  talking to `balbotd` over a Unix socket; it only translates frames to the same internal
  `drive`/`cmd` events used by WebSocket, so arbitration and limits are identical.

## 3.5 Arbitration and the control lease

Single driver at a time; any number of viewers.

1. First authenticated client that sends `lease request` while no lease is held becomes
   driver; the server increments `epoch` and returns it.
2. `drive` frames from a non-driver are ignored (error `NOT_DRIVER`). `estop` and `disarm`
   are accepted from **any** authenticated client.
3. Lease expires 1 s after the last `drive`/`ping` from the driver. An explicit
   `lease release` or higher priority transport (physical > WiFi > BLE) preempts.
4. Switching transports mid-drive (WiFi lost → BLE) is allowed only after the failsafe
   zeroed the velocity (≥ 250 ms), and requires an explicit re-`lease`, preventing runaway
   commands from a stale queue.
5. `epoch` is included in each `CMD_DRIVE` to the M4F; mismatches are dropped.

## 3.6 Failsafe summary (control path)

| Condition | Action |
|---|---|
| No `drive` frames for 250 ms | `balbotd` forces `vx=wz=0` (robot stands still) |
| WebSocket/BLE closed | same + lease released |
| `balbotd` silent for 500 ms | M4F ramps to zero velocity; 10 s later lie-down + disarm |
| Joystick released in the client | client sends explicit zeros at 50 Hz for 500 ms, then at 5 Hz |
| App backgrounded (RN) | client sends `disarm`-free zeros and disconnects after 3 s; reconnect is explicit |

## 3.7 Security

- Default posture: LAN/AP only, no cloud, no port forwarding.
- Pairing token: random 128-bit token printed as QR on the LCD at first boot; stored hashed.
  Admin vs. driver vs. viewer roles are encoded in the token.
- WPA2/WPA3 on the AP with a per-device random passphrase shown on the LCD.
- BLE: bonded, passkey display; Provisioning only over encrypted link.
- Rate limits and message size caps (3.3); the control channel parses nothing from untrusted
  clients except fixed-size numeric fields.
- Firmware limits on the M4F are the real safety boundary and cannot be raised remotely
  while `BALANCING`; the keys marked *safety* in the config table require `DISARMED`.
- Updates (OTA of M4F/PRU `.elf` and `balbotd`) signed with a project key (decision Q6).

> **Implemented:** the page below exists in `robot/linux/balbotd/balbotd/web/` (served at `/ui/`): dual sticks with touch, keyboard and gamepad,
> status chips, telemetry with a pitch plot, E-STOP, take/release control, arm/disarm/reset, MJPEG video. Not yet: tuning plots, calibration wizard,
> config editing. A `reset` message and `CMD_RESET` (0x06) were added so a latched fault can be cleared from the page.

## 3.8 Web UI

Single page, mobile-first:

- Video panel (MJPEG `<img>` first; WebRTC `<video>` later) with latency/fps overlay.
- Virtual dual joystick (left: speed, right: turn), also keyboard (WASD) and gamepad API
  support (the browser can use a USB/BT game controller; maps to the same `drive` message).
- Status bar: state, battery, RSSI, pitch, faults. Big **E-STOP** button.
- Tuning page (admin): live plots of pitch, rate, setpoint, output at up to 100 Hz,
  gain sliders with min/max clamps, save/revert; blackbox download.
- Calibration wizard (gyro, level, balance-point trim).

## 3.9 React Native app (later)

- Expo with development builds (needed for `react-native-webrtc` and BLE).
- Screens: Connect (discover via mDNS and BLE scan, add by IP), Drive (video + joysticks),
  Telemetry/Tuning, Settings (WiFi provisioning via BLE, rotate token).
- State management: Zustand; transport layer abstract (`WsTransport`, `BleTransport`) so UI
  does not know which channel is active.
- Tests: Jest unit tests for protocol/CRC/rate limiter/lease logic with the **same
  vectors** as the Python and C implementations (golden files in `shared/vectors/`),
  React Native Testing Library for screens, Detox/Maestro E2E against the simulator
  server (doc 6).
