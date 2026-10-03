# 2. Video streaming

## 2.1 Constraint that drives everything

The AM62x/AM6232 has **no hardware video encoder** (the AM62A variant has one). Two
dual-core A53 @ 1.4 GHz can software-encode low resolution H.264, but that burns the CPU
that also runs WiFi, USB and the web server. So the plan is:

1. **Let the camera compress.** Use a UVC camera that outputs **MJPEG** (nearly all) or
   **H.264** (e.g. Logitech C920-class, some ELP/Arducam modules). The A53 only forwards
   packets. Near-zero CPU.
2. Fall back to software x264 (`ultrafast`, `zerolatency`) only for 320x240–640x480 when
   WebRTC from an MJPEG-only camera is needed.

## 2.2 Phased delivery

| Phase | Transport | Client | Latency (typical, LAN) | CPU on A53 |
|---|---|---|---|---|
| V1 | MJPEG over HTTP (`multipart/x-mixed-replace`) from UVC MJPEG passthrough | browser `<img>`, RN `Image`/`WebView` | 100–250 ms | < 10 % |
| V2 | WebRTC with H.264 passthrough from a UVC H.264 camera | browser `RTCPeerConnection`, RN `react-native-webrtc` | 80–200 ms | < 10 % |
| V2b | WebRTC with software x264 from MJPEG source | same | 150–300 ms | 40–80 % of one core at 640x480@15 *(initial, to measure)* |
| V3 | adaptive bitrate / resolution, snapshot endpoint, recording | all | — | — |

Targets: the selected UVC camera (doc 8.4) offers MJPG 1280x720, 1920x1080 and 640x480, all at
30 fps (YUYV only 10 fps at 720p). V1 is 1280x720 MJPG passthrough; 640x480 MJPG is the
low-bandwidth and WebRTC source. 720p software x264 is not planned on the A53s.
Glass-to-glass latency goal < 250 ms; hard ceiling for usable driving is ~400 ms.

> **Implemented (V1):** `robot/linux/balbotd` serves `/api/v1/video/mjpeg` and `/snapshot`. Instead of go2rtc it runs
> `ffmpeg -f v4l2 -input_format mjpeg ... -c:v copy -f image2pipe -` itself and forwards the JPEG frames (never decoded), one camera process
> for any number of viewers, stopped when nobody watches. go2rtc remains the plan for WebRTC (V2). Not yet measured on the real camera: latency,
> CPU and the 720p/1080p USB bandwidth.

## 2.3 Pipeline

```
 USB camera (UVC) ──V4L2──► capture ─┬─► MJPEG HTTP endpoint (/video.mjpg)       V1
                                      ├─► H.264 passthrough ──► WebRTC (WHEP)     V2
                                      └─► (MJPEG → x264) ─────► WebRTC            V2b
```

Recommended implementation: **go2rtc** (single static binary, accepts `v4l2`/`ffmpeg:`
sources, serves MJPEG, WebRTC/WHEP, MSE, RTSP, HTTP API) managed by `balbot-video.service`,
reverse-proxied by `balbotd` so the clients see one origin, one port and one auth token.
Alternative: GStreamer (`v4l2src ! image/jpeg ! ...`) with `webrtcbin`, or MediaMTX. Pick go2rtc
first because it removes custom media code; revisit if packaging on Debian arm64 is a problem.

Camera setup (`v4l2-ctl`): fixed exposure/white balance modes where possible (auto exposure
causes frame-rate drops in dim light), `-c exposure_dynamic_framerate=0`, frame size and
fps pinned in a config file, camera node by `/dev/v4l/by-id/…` for stability.

## 2.4 Web server

| Endpoint | Method | Purpose |
|---|---|---|
| `/` | GET | single-page web UI (video + joystick + telemetry + settings), static files |
| `/api/v1/state` | GET | JSON: firmware version, robot state, battery, IP, RSSI |
| `/api/v1/config` | GET/PUT | gains, limits (admin token only) |
| `/api/v1/video/mjpeg` | GET | MJPEG stream (V1) |
| `/api/v1/video/whep` | POST | WebRTC WHEP offer/answer (V2) |
| `/api/v1/video/snapshot.jpg` | GET | single frame |
| `/ws` | WebSocket | control + telemetry (doc 3) |
| `/api/v1/wifi` | GET/PUT | scan, join, AP settings (admin) |
| `/api/v1/ota/*` | POST | firmware/app update (admin) |

Server stack: Python FastAPI + uvicorn behind no extra proxy; static UI built with plain
TypeScript + Vite (shared message types with the RN app through a `shared/` package).
mDNS name `balbot.local` via Avahi (`_http._tcp`, `_balbot._tcp`) so apps can discover it.

Video must never block control: the video service runs at `nice 5` on CPU1, `balbotd` is
`SCHED_FIFO` on CPU0, and the WebSocket control path uses its own small payloads.

## 2.5 WiFi (USB dongle)

- Dongle: pick an adapter with an **in-kernel driver** on 6.18, with both WiFi and BT, e.g.
  MediaTek **MT7921AU** (`mt7921u` + `btusb`). Realtek RTL8821CU/8812BU generally need
  out-of-tree DKMS, which complicates the CI image; avoid unless necessary. Confirm on the
  real board in M0.
- Prefer **5 GHz** to avoid 2.4 GHz contention with Bluetooth and busy channels.
- Modes managed by NetworkManager:
  - *STA* (join home/lab network) — default when a known network is visible.
  - *AP fallback* (`balbot-XXXX`, WPA2/WPA3, fixed 192.168.50.1/24, DHCP/DNS via
    NetworkManager shared mode) — automatic after 20 s without a known network, and
    forced by a hold-button at boot. Range outdoors is the main reason to keep AP mode.
- USB budget: camera MJPEG 640x480@30 ≈ 15–40 Mbit/s plus WiFi dongle traffic on one USB 2.0
  host (480 Mbit/s raw, ~280 usable). Use a powered hub only if current demands it; keep
  camera isochronous endpoints on a hub with a transaction translator that supports them
  (test in M6). The cape's host port or a USB-C OTG adapter is the host connection; the
  device port is used for development (the existing USB gadget service).
- Power: dongle + camera draw 0.5–0.8 A peak from 5 V. Budget the 5 V buck accordingly and
  keep it separate from motor power with bulk capacitance to avoid brown-outs on motor
  stalls.

## 2.6 Streaming to React Native (later)

| Feature | Library | Notes |
|---|---|---|
| MJPEG view | `react-native-webview` or `<Image>` with a refreshing URL | simplest; works on iOS and Android with no native code |
| WebRTC video | `react-native-webrtc` | requires a development build (not Expo Go); use WHEP signalling to the same server |
| BLE | `react-native-ble-plx` | iOS needs `NSBluetoothAlwaysUsageDescription`, Android 12+ `BLUETOOTH_SCAN/CONNECT` permissions |
| Discovery | `react-native-zeroconf` | finds `_balbot._tcp` |
| Joystick | `react-native-gesture-handler` + Reanimated | 50 Hz command loop on the JS thread must not stall the UI; run it on a timer, not on render |
| iOS local network | `NSLocalNetworkUsageDescription`, Bonjour services in `Info.plist` | otherwise discovery silently fails on iOS 14+ |
| Cleartext HTTP | Android `usesCleartextTraffic` for the LAN, or self-signed TLS pinned in app | decision in doc 7 (Q4) |

The app and the browser UI share a TypeScript package for the protocol (message types,
CRC8, rate limiter, state machine for connect/reconnect) so both are tested by one Jest suite.

## 2.7 Failure behaviour and QoS

- Stream stall or reconnect never changes robot state. UI shows a "no video" overlay; control
  is independent.
- Server drops frames instead of queueing (single-frame buffer for MJPEG, `leaky=downstream`
  in GStreamer) so latency does not grow.
- Telemetry overlay timestamps (robot monotonic clock in the frame metadata or a parallel WS
  message) allow measuring glass-to-glass latency in test (doc 6, test V-3).
- Bandwidth adaptation (V3): the client reports RTT/decode drops over the WS; `balbotd`
  steps resolution/fps (v4l2 reconfigure, restarts the stream in < 1 s) among
  1280x720@30 → 1280x720 with frames dropped to 15 fps by the server → 640x480@30 → 640x480 dropped to 15 fps (the camera only offers 30 fps in MJPG; profiles in doc 8.4).
