# Two-wheel self-balancing robot on PocketBeagle 2

Design and development plan for a self-balancing, remotely driven, video-streaming
robot built on the PocketBeagle 2 (TI AM62x). Status: **design / pre-implementation**.
Nothing in this folder is built or tested yet; every number marked *(initial)* is a
starting value to be replaced by measurement.

| # | Document | Covers |
|---|---|---|
| 1 | [System architecture](01-system-architecture.md) | What runs on A53 Linux, M4F Zephyr, PRU0/PRU1; IPC; timing; safety chain; pin budget |
| 2 | [Video streaming](02-video-streaming.md) | USB camera, MJPEG/WebRTC, web server, WiFi, later React Native |
| 3 | [Remote control](03-remote-control.md) | WebSocket + BLE protocols, arbitration, failsafe, web/mobile apps |
| 4 | [MPU6500 handling](04-imu-mpu6500.md) | Bus, registers, init, calibration, filtering, sensor fusion |
| 5 | [Balance control](05-balance-control.md) | Plant model, PID cascade, LQR option, state machine, motion control, tuning |
| 6 | [Test plan and CI](06-test-plan-ci.md) | Unit/sim/HIL tests, coverage targets, GitHub Actions design |
| 7 | [Development plan](07-development-plan.md) | Milestones, repo layout, BOM, risks, open questions |

## Goals

1. Balance on two DC-geared wheels with pitch error under ±1° at rest and recovery from a
   ~5° push within ~1 s.
2. Drive (forward/back/turn) from a browser first, then from an iOS/Android React Native app.
3. Stream live video over WiFi (USB dongle) to the same clients; control also available over
   Bluetooth LE as a fallback and for provisioning.
4. Hard real-time control that **does not depend on Linux**: a Linux crash, WiFi drop or
   video stall must never be able to make the robot fall unexpectedly.

## Architecture at a glance

```
 Phone / browser ──WiFi──► A53 Linux (balbotd, web server, video) ──rpmsg──► M4F Zephyr
        └──BLE (GATT)──►      │ USB: camera, WiFi+BT dongle                    │ IMU (SPI) 1 kHz
                              │                                                │ balance + safety
                              │                                   shared RAM ▼ │
                              │                                  PRU1: motor PWM + watchdog
                              │                                  PRU0: quadrature encoders
```

## Verified facts vs. assumptions

Verified (repo or public documentation):

- AM6232 PocketBeagle 2: dual Cortex-A53, one Cortex-M4F (up to 400 MHz), dual-core PRUSS
  (up to 333 MHz). Zephyr supports the M4F, loaded from Linux via remoteproc.
  ([PocketBeagle 2, Zephyr docs](https://docs.zephyrproject.org/latest/boards/beagle/pocketbeagle_2/doc/pocketbeagle_2.html),
  [BeagleBoard](https://www.beagleboard.org/boards/pocketbeagle-2)). Note: Zephyr docs and the
  Zephyr PR were reachable only via search snippets from this environment; confirm board target
  name and memory map when starting M2.
- The repo targets Debian 13.7 IoT, kernel 6.18.x-k3 (`ci/target.env`).
- The AM62x has **no hardware video encoder** (that is AM62A). Video must be camera-side
  compression (MJPEG/H.264 from a UVC camera) or software x264 on the A53s.
- PocketBeagle 2 has **no on-board WiFi/Bluetooth**; both come from USB dongles, hence the
  USB hub in the BOM.
- The GamePup A4 overlays in this repo already consume many P1/P2 header pins
  (see the pin budget in doc 1); the robot pin map was chosen around them.

Assumptions that must be confirmed during M0/M1 (tracked in doc 7):

- Which header pins can be muxed to PRU0/PRU1 R30/R31 and to the MCU-domain SPI/GPIO.
- That the M4F can read/write PRU shared RAM through the interconnect without a firewall
  block under the Beagle device tree.
- Out-of-tree/in-tree driver support for the chosen WiFi+BT dongle on kernel 6.18-k3.

## Branch / process note

Developed on `docs/balance-robot-design`, branched from `cursor/ili9341-320x240-dea5`.
No pull request has been opened.
