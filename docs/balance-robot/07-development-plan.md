# 7. Development plan, repository layout, BOM, risks

## 7.1 Proposed repository layout (not created yet)

```
robot/
  core/            portable C: imu_filter, pid, lqr, balance_ctrl, state_machine, proto, cal
    include/ src/ tests/ CMakeLists.txt
  firmware-m4/     Zephyr app (west manifest, boards/, drivers/mpu6500, src/, tests/)
  firmware-pru/    pru0_qdec/, pru1_pwm/, common/ (mailbox layout header shared with M4F)
  linux/
    balbotd/       Python supervisor, REST/WS, rpmsg bridge, BLE bridge, arbiter
    systemd/       balbot-*.service
    overlays/      robot pinmux + remoteproc/PRU carveout overlay (.dts, same style as overlays/)
    video/         go2rtc config, camera setup
  web/             Vite + TypeScript UI
  app/             React Native app (Expo dev build)
  shared/          TS protocol package + golden vectors (JSON) used by C/Py/TS tests
  sim/             Python plant, scenarios, ctypes binding to core
  tools/           lqr_design.py, imu_ref.py, log viewer, trace_check.py, calibration scripts
  config/          robot-<name>.yaml (physical params, gains, limits) -> generated params.h
.github/workflows/ robot-ci.yml, robot-nightly.yml
docs/balance-robot/ this folder (+ requirements.md in M0)
```

The existing GamePup content stays untouched; the robot reuses its conventions (overlay
style in `overlays/`, `ci/target.env` pinning, installer scripts, USB gadget service for
development access, ILI9341 and OLED UI).

## 7.2 Milestones

Estimates are for one engineer with the hardware in hand, in calendar weeks; each milestone
has an exit criterion that is a test, not a feeling.

| M | Title | Work | Exit criterion | Wk |
|---|---|---|---|---|
| M0 | Hardware bring-up and pin map | order parts; confirm PB2 header pins for PRU0/PRU1/MCU SPI/GPIO; USB hub + WiFi/BT dongle + camera enumerate on kernel 6.18-k3; `requirements.md`; repo skeleton + CI skeleton | pin table verified with scope; `lsusb`, `iw`, `bluetoothctl`, `v4l2-ctl` work; CI green on empty tests | 2 |
| M1 | Toolchains and "hello" on every core | Zephyr SDK/west; blink + shell on M4F via remoteproc; PRU toggles a pin; rpmsg echo A53⇄M4F; M4F⇄PRU mailbox read/write | echo round-trip stats; M4F reads PRU counter; documented boot scripts | 2 |
| M2 | MPU6500 driver + calibration | SPI driver on M4F, DRDY ISR, health checks, unit tests with mock, gyro bias calibration, blackbox ring | HIL-BENCH static accuracy; IMU-1…8, CAL-1…5 pass; 1 kHz sampling jitter < 20 µs | 2 |
| M3 | Estimator and offline sim | complementary + Kalman + Mahony in `core`; Python reference; golden vectors; tilt-table test; sim plant v1 | EST-1…8 pass; tilt table < 0.5° static | 2 |
| M4 | Motors, encoders, PRU firmware | PRU0 qdec, PRU1 PWM + failsafe, driver wiring, sign tests; parameter identification of the robot | HIL-3, HIL-4 pass; model parameters in YAML | 2 |
| M5 | Balance v1 (PID cascade) | `balance_ctrl`, state machine, safety thread, tuning shell; SIM-1…12; first balance on a tether | R-1, R-2, R-3 pass; recorded tuned gains | 3 |
| M6 | Linux supervisor + web control | `balbotd`, lease/arbiter, WS protocol, web UI joystick, telemetry plots, systemd units, WiFi STA/AP | R-4, R-5; N-1, N-3; UT-PY complete | 3 |
| M7 | Video V1 (MJPEG) | camera service, endpoint, UI panel, latency overlay | V-1…V-3 on LAN and AP; R-9 (no impact on loop) | 1.5 |
| M8 | BLE control + provisioning | BlueZ GATT service, bonding with passkey on LCD, WiFi provisioning, BLE fallback | N-5 (BLE part); control over BLE works | 2 |
| M9 | React Native app | connect/discover, drive, telemetry, MJPEG then WebRTC, BLE transport | Maestro E2E; device matrix pass | 4 |
| M10 | WebRTC (V2/V2b), adaptive quality | H.264 passthrough camera or x264; bitrate steps | V-1 target met with WebRTC | 2 |
| M11 | LQR + advanced motion | LQR design pipeline, gain scheduling, position hold, optional self-righting | SIM and R tests with LQR ≥ PID metrics | 3 |
| M12 | Hardening and release | security tests, OTA, soak tests, docs, field test | N-4, N-6, N-7, N-8 pass; release `robot-v1.0.0` | 3 |

Critical path: M0 → M1 → M2 → M3/M4 → M5. M6–M10 can overlap with M5 once the control
interface (`CMD_DRIVE`, telemetry) is frozen; **freeze it at the end of M1** (`proto v1`).
Total ≈ 30 weeks sequentially; ≈ 18–20 weeks with two people (firmware/control vs.
Linux/apps).

## 7.3 Initial BOM (indicative; verify each part)

| Item | Suggested | Notes |
|---|---|---|
| Compute | PocketBeagle 2 | already available |
| Display (status) | GamePup A4 cape's ILI9341 and/or SH1106 OLED | reuse from this repo |
| IMU | MPU-6500 breakout with SPI broken out | alternative: ICM-42688 later |
| Motors | 2 × 12 V DC gearmotor with Hall quadrature encoder (e.g. 37 mm, 150–330 rpm, ≥ 11 CPR motor-side) | torque and no-load speed drive max speed; encoder must be 3.3 V compatible or level-shifted |
| Motor driver | dual H-bridge, ≥ 3 A continuous per channel, PWM up to 20 kHz (e.g. TB6612-class small, or DRV8874/VNH-based for stall margin) | PWM+DIR inputs, enable pin wired to E-stop |
| Wheels | 65–80 mm rubber | grip matters |
| Battery | 3S Li-ion/LiPo 11.1 V (or 2S 7.4 V with lower speed) with BMS and fuse | low-voltage cutoff in firmware |
| Power | 5 V ≥ 3 A buck, separate from motor path, bulk caps | Pi-style supply noise isolation; common ground star |
| Power monitor | INA226 on battery rail | I2C |
| USB | small USB 2.0 hub (+ OTG adapter) | check PB2 host port current |
| Camera | UVC MJPEG (and H.264 for V2) | fixed-focus, wide FOV |
| WiFi+BT | MT7921AU-class dongle | in-kernel drivers; 5 GHz |
| Safety | arming switch, E-stop button wired to driver enable and M4F GPIO | hardware path |
| Chassis | 3D printed or laser-cut, battery low, skids/bumpers front and rear | for safe falls |
| Debug | USB-UART for M4F shell, logic analyser, scope | |

## 7.4 Risks and mitigations

| ID | Risk | Likelihood | Impact | Mitigation / fallback |
|---|---|---|---|---|
| R1 | PB2 header does not expose the needed PRU or MCU-domain pins | M | H | M0 pin audit first; fallback: encoders on M4F GPIO interrupts, PWM on eHRPWM owned by M4F, add external watchdog |
| R2 | Zephyr M4F support on PB2 incomplete (SPI/GPIO/IPC drivers) | M | H | evaluate in M1; fallback: TI MCU+ SDK / bare-metal driver for SPI, keep Zephyr for the app layer |
| R3 | M4F cannot access PRU DRAM (firewall/address mapping) | L–M | H | check in M1; fallback: M4F owns motor timers; or use PRU←→M4F via rpmsg-less mailbox registers/IPC; worst case use A53 kernel driver with an `isolcpus` core (weaker) |
| R4 | rpmsg/remoteproc image or DT carveouts differ on the Debian IoT 6.18-k3 image | M | M | prototype in M1 against the pinned image in `ci/target.env`; capture DT overlay in repo |
| R5 | WiFi/BT dongle driver issues on 6.18-k3 | M | M | pick a dongle with in-kernel driver; M0 test; keep a second candidate |
| R6 | USB 2.0 bandwidth/CPU limits video + WiFi | M | M | MJPEG passthrough, lower res, camera H.264; measure in M7 |
| R7 | Gyro-DLPF + loop delay prevents stable gains | L | H | experiments E2 (lower DLPF latency), faster loop, tune estimator |
| R8 | Motor dead-zone/backlash makes balance twitchy | M | M | better gearmotors, dead-zone compensation, encoder-based friction model |
| R9 | WiFi latency/jitter makes driving poor | M | M | AP mode on 5 GHz, command timeout, lower video resolution |
| R10 | RN WebRTC/BLE native build complexity | M | M | MJPEG-in-WebView as first mobile milestone; BLE for control only |
| R11 | Safety: robot injures people/self | L | H | speed limits, hardware E-stop, bumpers, test rig, sim-verified failsafes; never test with unsecured wheels at speed |
| R12 | Single-person maintenance of 4 codebases | M | M | shared protocol vectors, strong CI, monorepo, small interfaces |

## 7.5 Experiments to run early

| ID | Experiment | Decides |
|---|---|---|
| E1 | rpmsg round-trip latency/jitter distribution, 10⁶ messages with CPU stress on A53 | whether rpmsg is OK for 50–100 Hz traffic |
| E2 | gyro DLPF 184 Hz/1 kHz vs. 250 Hz/8 kHz with firmware averaging | phase lag budget |
| E3 | M4F loop WCET and jitter with PRU mailbox access | headroom, filter choice |
| E4 | camera MJPEG vs. H.264 passthrough CPU and latency on A53 | video plan |
| E5 | WiFi 2.4 vs 5 GHz with BT active (coexistence) | radio config |
| E6 | motor start voltage and encoder CPR at speed | dead zone and speed estimation limits |

## 7.6 Open questions for the project owner

| ID | Question | Default if unanswered |
|---|---|---|
| Q1 | Exact motors/driver/battery already owned? | as in 7.3; revisit after M0 |
| Q2 | Is the GamePup cape staying on the robot (display, buzzer) or is it a clean build? | keep only ILI9341 + optional OLED |
| Q3 | Camera model (MJPEG-only or H.264)? | MJPEG V1, test H.264 camera for V2 |
| Q4 | Transport security: plain HTTP on private AP/LAN or self-signed TLS? | TLS with certificate fingerprint pinned in the app; plain HTTP only in dev builds |
| Q5 | Is a self-righting/stand-up from lying position required? | out of scope for v1 (operator lifts) |
| Q6 | Signed OTA required? | yes for M12; signing key outside the repo |
| Q7 | License for the robot code (the repo is GPL-2.0 for kernel-related parts; LICENSE file present) | GPL-2.0-only to match repo unless told otherwise |
| Q8 | Is a dedicated second PB2 / rig available for HIL? | needed by M5 |

## 7.7 Definition of done for v1.0

- Balances > 10 min, recovers from standard push, driven from browser and iOS/Android app.
- Video ≥ 25 fps, p95 latency < 400 ms on 5 GHz.
- Failsafes verified (link loss, Linux stall, M4F crash, e-stop, low battery).
- CI green incl. nightly HIL; coverage and timing gates met.
- Documentation updated with measured values replacing every *(initial)* number.
