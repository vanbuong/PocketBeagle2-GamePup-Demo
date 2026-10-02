# 1. System architecture, task partitioning and inter-core communication

## 1.1 Design principles

1. **Closed-loop balance never touches Linux.** Sensing, estimation, control and motor
   output live on the M4F and PRUs. Linux supplies *intent* (speed/turn setpoints) and
   *telemetry sinks*, nothing in the 500 Hz loop.
2. **Every layer has a watchdog on the layer above it** and degrades to a safer state on
   its own (section 1.6).
3. **Lossy, non-blocking IPC.** The M4F never waits for Linux; if a queue is full it drops
   telemetry, never control.
4. **One writer per datum.** Each shared memory field has exactly one producer.
5. **Portable core.** Algorithms (filter, PID/LQR, state machine, protocol codec) are plain C
   with no OS or HAL calls, so they run unchanged in host unit tests, the Python simulator
   and on the M4F (doc 6).

## 1.2 Responsibility map

| Domain | OS | Responsibilities | Rate / deadline |
|---|---|---|---|
| **A53 (2 cores)** | Debian 13 Linux | WiFi (STA/AP), BLE GATT peripheral, web/WebSocket server, video capture and serving, `balbotd` supervisor (command arbitration, rpmsg bridge, logging, config), remoteproc firmware loading, LCD/OLED UI (existing GamePup), OTA update | soft real-time; 50 Hz control commands, 10–100 Hz telemetry |
| **M4F** | Zephyr RTOS | MPU6500 driver (SPI + DRDY IRQ), calibration, attitude estimation, balance cascade (angle/speed/yaw), safety state machine, battery monitor, fault latch, command timeout, rpmsg endpoint, heartbeat to PRU1 | IMU 1 kHz, control 500 Hz, speed loop 100 Hz, telemetry 100 Hz; jitter budget < 100 µs |
| **PRU0** | bare-metal (clpru C/asm) | Quadrature decode of 2 wheel encoders (4x), per-wheel signed count and edge timestamp, direction-error counter | polls every ~30 ns; publishes counts continuously |
| **PRU1** | bare-metal | 20 kHz PWM and direction outputs for 2 motor drivers, dead-time / brake logic, duty slew limit, **hardware failsafe: brake if the M4F heartbeat is stale > 10 ms** | one 50 µs period; duty latched at period start |

### Why each assignment

- **M4F for the loop.** Deterministic interrupt latency (no cache/MMU/scheduler), single-precision
  FPU (the whole attitude + PID update is a few hundred cycles, ≈ 1–2 µs at 400 MHz) and it
  keeps running if Linux panics. Zephyr gives drivers, threads, logging, `ipc_service`/OpenAMP
  and `west twister` unit-test tooling.
- **PRU0 for encoders.** Pulses from Hall encoders on geared motors are low rate
  (thousands of edges per second) but jitter-free capture removes missed-count errors that
  would corrupt the speed loop. A table-driven 4x decoder in PRU needs no interrupts. This
  could also be done with the AM62x eQEP peripherals; the PRU choice frees them and keeps
  the encoder path independent of Linux ownership of main-domain peripherals. *Fallback if
  PRU pins are not routable:* M4F GPIO interrupts with a state table, accepted up to
  ~20 kHz edge rate.
- **PRU1 for motor outputs and the last-resort watchdog.** Cycle-exact PWM that continues
  and *fails safe* even if the M4F hangs. *Fallback:* M4F-owned eHRPWM through a Linux DT
  pin/clock hand-off; loses the independent watchdog (add an external supervisor MCU/timer).
- **A53 for everything with an OS dependency** (network stacks, V4L2, TLS, Bluetooth
  host). Linux scheduling jitter is irrelevant because nothing hard real-time is there.

### Optional PRU extras (not in v1)

WS2812 status LEDs, ultrasonic/ToF echo timing, or a second IMU capture on PRU if the M4F
ever becomes the bottleneck.

## 1.3 Data flow

```
                           A53 / Linux                                M4F / Zephyr                  PRU
 ┌──────────┐ WS/BLE  ┌────────────────────────┐   rpmsg (cmd, evt)   ┌──────────────────────┐
 │ client   │◄───────►│ balbotd                │◄────────────────────►│ app thread (rpmsg)   │
 └──────────┘         │  - auth, lease/arbiter │   seqlock telemetry  │ control ISR/thread   │
 ┌──────────┐ HTTP    │  - rate limit, logging │◄─────────────────────│  500 Hz              │
 │ video    │◄───────►│ video service (go2rtc/ │   (carveout RAM)     │ imu_task (DRDY IRQ)  │
 └──────────┘         │  GStreamer)            │                      └─────┬──────────▲─────┘
                      └────────────────────────┘                    duty/dir│          │counts
                                                                       ┌─────▼──────────┴─────┐
                                                                       │ PRU shared DRAM      │
                                                                       │ PRU1 PWM │ PRU0 QEP  │
                                                                       └──────────────────────┘
                                                             MPU6500 ◄── SPI (M4F), INT ──► M4F GPIO
```

### Control cascade timing (per 1 ms IMU tick)

| t (µs) | Event |
|---|---|
| 0 | MPU6500 INT asserts (data ready). M4F GPIO ISR timestamps with the cycle counter, gives a semaphore. |
| < 20 | `imu_task` (highest-priority thread, or workqueue at prio 0) starts SPI burst read of 14 bytes (≈ 30 µs at 4 MHz). |
| ~60 | Raw to SI units, bias/scale applied, attitude filter updated (1 kHz). |
| every 2nd tick | Balance loop (500 Hz): read PRU encoder snapshot, run angle PD(+I), mix, write duty/dir + heartbeat counter to PRU1 mailbox (≈ 10 µs). |
| every 10th tick | Speed/yaw loop (100 Hz) updates pitch setpoint and turn term. |
| every 10th tick | Telemetry snapshot written to the seqlock block (never blocks). |
| 50 µs boundary | PRU1 latches the new duty at the next PWM period start (max added latency 50 µs). |

Total sensor-to-actuator latency budget: **≤ 2 ms** (1 ms sample + 0.1 ms bus/compute +
up to 1 ms to next control tick + 0.05 ms PRU). A gyro DLPF of 184 Hz adds ~2.9 ms of
group delay; this dominates and is accounted for in the tuning (doc 5, section 5.9).

## 1.4 Zephyr task design on the M4F

| Thread / context | Priority | Period | Stack | Notes |
|---|---|---|---|---|
| GPIO ISR (IMU DRDY) | IRQ | event | — | timestamp, `k_sem_give`, nothing else |
| `imu_ctrl` thread | cooperative prio −2 | 1 kHz (sem) | 2 KB | SPI read, fusion, runs control every 2nd cycle. Single thread avoids queue latency and priority inversion; the loop is < 30 µs so it fits in the 1 ms budget with > 95 % idle margin |
| `safety` thread | preemptive prio 1 | 1 kHz timer | 1 KB | independent checks: IMU data-age, loop overrun, tip angle, battery, PRU1 liveness; can force motor cut |
| `rpmsg` thread | prio 5 | event | 4 KB | decode commands, validate CRC and sequence, update command mailbox, send events |
| `telemetry` thread | prio 7 | 100 Hz | 2 KB | copies snapshot to ring, sends rpmsg summary if buffer available |
| `battery` thread | prio 8 | 10 Hz | 1 KB | INA226 over I2C (or ADC divider) |
| idle / shell | lowest | — | — | Zephyr shell on UART for bring-up and tuning |

Rules: no dynamic allocation after init; no `printk` in the IMU path; all `k_mutex` use
outside `imu_ctrl`; command data handed to `imu_ctrl` through a single-writer struct guarded
by a sequence counter, never a mutex. Loop execution time and period jitter are histogrammed
and exported in telemetry (used as a CI/HIL pass criterion).

## 1.5 Inter-processor communication

Three independent channels, chosen by data type.

### A53 ⇄ M4F: rpmsg (OpenAMP) — commands, events, configuration

Linux uses `rpmsg_ctrl`/`rpmsg_char` (device node `/dev/rpmsgN` created for an endpoint named
`balbot-ctl`); Zephyr uses `ipc_service` with the OpenAMP backend. Memory for vrings and
buffers comes from reserved-memory carveouts in the Linux DT for the M4F remote processor.
Firmware is placed in `/lib/firmware/` and started with
`echo <fw> > /sys/class/remoteproc/remoteprocN/firmware; echo start > .../state`
(find `N` by matching `name` files; ships as a systemd unit `balbot-m4.service`).

Frame format (little endian, 8-byte header, max payload 480 B):

| Field | Size | Meaning |
|---|---|---|
| `magic` | u8 | `0xB5` |
| `ver` | u8 | protocol version (1) |
| `type` | u8 | message id (table below) |
| `flags` | u8 | bit0 = ack requested |
| `seq` | u16 | sender sequence, wraps; receiver detects gaps |
| `len` | u16 | payload bytes |
| payload | len | type-specific |
| `crc16` | u16 | CRC-16/CCITT-FALSE over header+payload |

| Dir | Type | Id | Payload |
|---|---|---|---|
| A→M | `CMD_DRIVE` | 0x01 | `i16 v_cmd` (mm/s), `i16 w_cmd` (mrad/s), `u8 mode`, `u8 lease_epoch` |
| A→M | `CMD_ARM` / `CMD_DISARM` | 0x02/0x03 | none (DISARM always honoured) |
| A→M | `CMD_ESTOP` | 0x04 | none, latches `FAULT` |
| A→M | `CMD_HEARTBEAT` | 0x05 | `u32 uptime_ms` |
| A→M | `CFG_SET` / `CFG_GET` | 0x10/0x11 | `u16 key`, `f32 value` (gains, limits, trims) |
| A→M | `CFG_SAVE` | 0x12 | persist to settings (NVS/flash on SD via Linux file) |
| A→M | `CAL_START` | 0x20 | `u8 kind` (gyro, accel-level, accel-6pos) |
| M→A | `EVT_STATE` | 0x80 | state-machine state, fault bits |
| M→A | `TLM_FAST` | 0x81 | `u32 t_us`, pitch, pitch rate, yaw rate, v, u_L, u_R, vbat (10 × i16/f32) |
| M→A | `TLM_SLOW` | 0x82 | loop timing stats, temp, calibration state, counters |
| M→A | `CFG_VAL` | 0x90 | reply to `CFG_GET` |
| M→A | `CAL_RESULT` | 0xA0 | bias / scale values |

Latency of rpmsg from Linux user space is typically ~100 µs–1 ms with occasional spikes;
acceptable because the data it carries is 50–100 Hz and has deadlines of ≥ 100 ms.

### A53 ⇄ M4F: shared-memory seqlock block — high-rate blackbox

A reserved 64 KB region holds a ring of 64-byte records written at 1 kHz by the M4F
(timestamp, raw gyro/accel, pitch estimate, setpoint, command, encoder counts). A seqlock
counter lets `balbotd` read without locking the producer. Linux maps it through the
remoteproc carveout (`/dev/mem` is avoided; use a UIO or `remoteproc` memory-region
mapping). Used for tuning plots and post-fall analysis. Losing records is acceptable.

### M4F ⇄ PRU: shared PRU data RAM mailbox

PRU DRAM (8 KB per PRU) is mapped into the M4F address space (verify, risk R3 in doc 7).

PRU1 mailbox (written by M4F, read by PRU1):

| Offset | Field | Notes |
|---|---|---|
| 0x00 | `u32 heartbeat` | M4F increments every control tick |
| 0x04 | `i16 duty_l`, `i16 duty_r` | −1000…+1000 (‰ of PWM period) |
| 0x08 | `u8 mode` | 0 coast, 1 drive, 2 brake |
| 0x0C | `u16 slew_limit` | ‰ per period |
| 0x10 | `u32 pwm_period_ticks` | set once at init (20 kHz) |

PRU1 status (written by PRU1): `u32 applied_seq`, `u8 failsafe_active`, `u32 periods_count`.

PRU0 output (written by PRU0, read by M4F):

| Offset | Field |
|---|---|
| 0x00 | `i32 count_l` |
| 0x04 | `i32 count_r` |
| 0x08 | `u32 last_edge_ts_l`, `u32 last_edge_ts_r` (IEP counter) |
| 0x10 | `u32 seq` (incremented before and after update, seqlock) |
| 0x14 | `u16 illegal_transitions` (diagnostic, counts invalid Gray-code jumps) |

Speed estimate on the M4F = Δcount / Δt over the control period (with edge timestamps for
low-speed refinement). Count is 32-bit so wrap is a non-issue.

### Linux ⇄ PRU

None in operation. Linux only loads PRU firmware through `pru_rproc` at boot and may read
status registers for diagnostics through debugfs.

## 1.6 Safety chain

| Fault | Detected by | Reaction |
|---|---|---|
| Client link lost (WS/BLE) | `balbotd`: no `CMD_DRIVE` for 250 ms | send zero velocity (robot holds balance, stationary) |
| `balbotd` dead / Linux hung | M4F: no `CMD_HEARTBEAT` for 500 ms | ramp velocity to zero; after 10 s (configurable) controlled lie-down onto skids and disarm |
| Tip angle \|θ\| > 35° (initial) | M4F safety thread | motors brake, state `FALLEN` (latched until upright + re-arm) |
| IMU data age > 3 ms or WHO_AM_I/CRC fault | M4F | state `FAULT`, motors coast/brake |
| Control loop overrun (> 1.5 ms) 3× | M4F | `FAULT` |
| Battery below cutoff (e.g. 3.3 V/cell) | M4F | lie-down then disarm (avoids brown-out mid-balance) |
| M4F hung or crashed | PRU1: heartbeat stale > 10 ms | PWM → brake, `failsafe_active=1` |
| Encoder fault (illegal transitions, speed mismatch vs. IMU) | M4F | degrade to IMU-only mode (no speed loop) or fault |
| Emergency stop | any client, physical button on M4F GPIO | latched `FAULT`, brake |

A physical **arming switch / E-stop button** wired to an M4F GPIO and to the motor driver
enable line (hardware path, independent of firmware) is part of the BOM.

## 1.7 Linux services on the A53

| Unit | Purpose |
|---|---|
| `balbot-m4.service` | load/start M4F firmware via remoteproc, stop on shutdown |
| `balbot-pru.service` | load PRU0/PRU1 firmware before M4F so the mailbox exists |
| `balbotd.service` | supervisor (below), `SCHED_FIFO` 20, `mlockall`, CPU0 |
| `balbot-video.service` | camera capture and streaming, CPU1 |
| `balbot-ble.service` | BLE GATT peripheral through BlueZ D-Bus |
| `balbot-net.service` | WiFi mode manager (NetworkManager profiles: STA and AP fallback) |
| existing `gamepup-*` | optional LCD/OLED UI showing IP, battery, tilt, pairing PIN |

`balbotd` (Python 3.12 + `asyncio`, FastAPI/uvicorn; hot paths in C only if profiling
demands it) responsibilities: authenticate clients, own the *control lease* (one active
driver, arbitration in doc 3), clamp/rate-limit commands, forward at 50 Hz, republish
telemetry to WebSocket subscribers, record blackbox files, expose REST for config/OTA.
CPU placement: CPU0 for `balbotd` + IRQs of USB, CPU1 for video; cpufreq governor
`performance`.

## 1.8 Pin budget

### Pins already used by the GamePup A4 overlays in this repo (from `overlays/*.dts`)

| Header pin | Used for |
|---|---|
| P2.01 | ECAP2 backlight PWM |
| P2.02, 04, 06, 08 | left D-pad (GPIO0_45/46/47/48) |
| P2.18, 20, 22, 24 | right pad (GPIO0_53/49/63/51) |
| P2.17, P2.19 | LCD D/C, LCD reset |
| P2.25, 27, 29, 31 | SPI0 MOSI/MISO/CLK/CS0 (ILI9341) |
| P2.05, 07, 10, 11 | McASP2 (MAX98357A / INMP441) |
| P1.31, P1.35 | Select, Start |
| P1.29, P2.34 | eye LEDs |
| P1.33 | buzzer (EHRPWM2_B) |
| P1.36 | MAX98357 SD_MODE (GPIO1_28); also claimed by base-tree epwm2, see `PB2_BASE_TREE_EPWM2_P1_36_BUG.md` |
| P1.19, P1.34, P1.02 | EC11 encoder A/B/switch |
| P1.04, 06, 08, 10, 12, P2.03 | W5500 on SPI2 (optional overlay) |
| I2C2 | SH1106 OLED (0x3c), EEPROM (0x57) |

**Recommendation:** build the robot *without* the full GamePup cape (audio, buttons,
Ethernet overlay are not needed). Keep only the ILI9341 (SPI0) as a status display and
optionally the OLED on I2C2. This leaves the remaining header pins free for the robot.

### Required logical signals (physical pins assigned in M0)

| Signal | Count | Owner | Requirement |
|---|---|---|---|
| IMU SPI SCLK/MOSI/MISO/CS | 4 | M4F | an MCU-domain SPI (MCU_SPI0/1) if routed to the headers, otherwise main-domain SPI assigned to M4F |
| IMU INT | 1 | M4F | GPIO with interrupt capability reachable by M4F (MCU GPIO preferred) |
| Encoder L A/B, R A/B | 4 | PRU0 | pins muxable to PRU0 R31 inputs (3.3 V; level-shift if encoders run at 5 V) |
| Motor L PWM, DIR(/IN2), R PWM, DIR(/IN2) | 4 | PRU1 | pins muxable to PRU1 R30 outputs |
| Driver enable / STBY | 1 | hardware E-stop path | wired through E-stop and a GPIO |
| E-stop / arm button | 1 | M4F GPIO | input with pull-up |
| Battery sense | I2C (INA226) or 1 ADC | M4F | I2C addresses must not collide with I2C2 devices if shared |
| Status LED(s) | 1–2 | M4F or Linux | optional |

The PB2 PRU and MCU-domain pin availability is not confirmed from the repo and was not
reachable from the authoring environment. **M0 exit criterion:** a table with real header pin
numbers, pad names, mux modes and DT/firmware owner for each signal above, verified with
`pinmux` readback and a scope.

## 1.9 Boot sequence

1. Kernel boots; device-tree overlay sets pinmux and reserves M4F/PRU carveouts.
2. `balbot-pru.service` loads PRU firmware; PRU1 starts with outputs in **brake** (failsafe is
   the power-on state because the heartbeat is stale).
3. `balbot-m4.service` loads and starts the M4F image; M4F initialises SPI, probes the
   MPU6500 (WHO_AM_I = 0x70), runs gyro bias calibration (needs ~1 s stationary), enters
   `STANDBY`.
4. `balbotd` connects to rpmsg endpoint, reads firmware version, checks protocol version
   compatibility, publishes state.
5. Network and video services start independently; their failure does not block 2–4.
6. Operator lifts the robot upright (±5° for 1 s) and arms → `BALANCING`.

Total boot-to-ready target: < 30 s (Linux dominated). M4F + PRU can be started from U-Boot
for faster, Linux-independent start as a later optimisation.
