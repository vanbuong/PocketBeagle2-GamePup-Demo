# 8. Selected hardware: JGB37-520 + TB6612 + 720p USB camera

Chosen parts: **JGB37-520 gearmotors with quadrature (Hall) encoders**, **TB6612FNG dual
driver**, **720p UVC USB camera**. This page records what that choice implies for the rest
of the design. Datasheet figures below are from memory of the TB6612FNG datasheet and
typical JGB37-520 listings; seller data varies, so **read your motor label and measure**
(procedure in doc 5, 5.1).

## 8.1 JGB37-520 gearmotor with encoder

| Item | Typical value | Consequence |
|---|---|---|
| Rated voltage | 12 V DC (runs 6–12 V) | **3S 18650 pack (9.0–12.6 V, 11.1 V nominal) selected**; 2S would give ~60 % speed/torque |
| Gear ratio | 1:10 … 1:90 variants (e.g. 1:30 ≈ 330 rpm no-load at 12 V) | **Pick ratio first**: 1:20–1:30 suits a 65–80 mm wheel; ≥ 1:50 is too slow to recover from a push |
| Encoder | Hall, 2 channels, **11 PPR per channel on the motor shaft** → 44 counts/rev at 4x | counts per wheel rev = `44 × ratio` (1:30 → 1320) |
| Encoder power / levels | 3.3–5 V supply; outputs swing to the supply voltage | **power encoders from 3.3 V** → direct connection to PRU0 inputs, no level shifter |
| Stall current | often 3–6 A at 12 V (varies) | exceeds TB6612 limits, see 8.2 |
| Wiring | 6 wires: M+, M−, GND, VCC(encoder), A, B | add 100 nF at encoder connector, twisted pair A/B, keep away from motor leads |

### Encoder resolution and speed estimation

Example 1:30, 65 mm wheel: 1320 counts/rev → **0.155 mm per count**.
Max no-load wheel speed ≈ 330 rpm × π × 0.065 ≈ 1.1 m/s (≈ 1.0 m/s at 11.1 V), edge rate at
that speed ≈ 7 kcounts/s per wheel (trivial for a PRU, still feasible on M4F GPIO IRQs).

Quantisation matters for the control rate: one count in a 2 ms (500 Hz) window is
0.077 m/s, far too coarse. Therefore:

- The **inner 500 Hz loop uses no encoder data** (angle + gyro only).
- Speed is computed at **100 Hz** (10 ms window: 0.0155 m/s per count) and low-pass filtered.
- Below ~0.15 m/s use the **M/T method**: PRU0 stores the IEP timestamp of each edge, speed =
  (counts between first and last edge in window) / (time between those edges). Resolution
  then stays constant at low speed. Documented as `PRU0` output `last_edge_ts_*` in doc 1.
- Doc 4 (4.8) and doc 5 (5.4) now reference this; unit tests EST-9/10 cover it (6.3).

| Ratio | Counts/wheel rev | mm/count (65 mm) | No-load wheel speed @12 V | Verdict |
|---|---|---|---|---|
| 1:10 | 440 | 0.46 | ~1000 rpm (3.4 m/s) | too little torque, coarse encoder |
| 1:20 | 880 | 0.23 | ~500 rpm (1.7 m/s) | good if torque suffices |
| **1:30** | **1320** | **0.155** | **~330 rpm (1.1 m/s)** | **recommended starting point** |
| 1:50 | 2200 | 0.093 | ~200 rpm (0.7 m/s) | marginal speed headroom |

## 8.2 TB6612FNG driver

| Item | Value (verify datasheet) | Consequence |
|---|---|---|
| Motor supply VM | 4.5–13.5 V | the 3S 18650 pack is 12.6 V fully charged: little margin. Fit a TVS and 470 µF+ close to the board, never hot-plug the battery, check spikes on a scope (doc 9.3), or use a 10 V motor rail / 2S |
| Current per channel | **1.2 A continuous, 3.2 A peak** (short pulses) | below JGB37-520 stall current; see mitigations |
| Logic | 2.7–5.5 V, so **3.3 V from PRU1 pins directly** | no level shifter |
| PWM | up to 100 kHz | we use 20 kHz |
| On-resistance | ~0.5 Ω (high+low) | adds to motor R in the model: `R_total = R_motor + R_on`; costs ~0.5 V of headroom at 1 A |
| Protection | thermal shutdown, no current sense, no current limit | firmware and hardware mitigations below |
| Inputs per channel | `PWM`, `IN1`, `IN2`, shared `STBY` | **6 PRU1 outputs + STBY**, not 4 (doc 1, 1.8 updated) |

Drive truth table (per channel): `IN1=H, IN2=L, PWM=H` forward, `IN1=L, IN2=H, PWM=H`
reverse, `PWM=L` (with IN1≠IN2) **short brake**, `IN1=IN2=H` short brake, `IN1=IN2=L` stop (coast),
`STBY=L` standby (outputs high-Z).

PRU1 implementation: set direction with `IN1/IN2`, put the 20 kHz PWM on the `PWM` pin.
Off-time is then short-brake (slow decay), which gives a nearly linear duty→voltage→torque
relation, good for the balance loop. Failsafe state: `IN1=IN2=H, PWM=H` (brake).
`STBY` has a hardware pull-down and is driven through the E-stop switch, so any loss
of logic power or an E-stop press puts the driver in standby (coast, safest for electronics).

### Current-limit strategy (no sensor on the board)

1. **Estimated motor current** in firmware: `I ≈ (u − Ke·ω_motor) / R_total` per wheel,
   `ω_motor` from the encoders. Command is scaled so `|I| ≤ 2.4 A` for ≤ 100 ms bursts and
   ≤ 1.0 A average over 2 s *(initial; tune to measured thermals)*.
2. **Stall detect:** |duty| > 60 % for > 200 ms with wheel speed < 5 % of expected → fault, motors off.
3. **Battery-rail INA226** (already in the BOM) measures total current; fault above 4 A for 100 ms.
4. **Hardware option:** parallel the two bridges of each channel (A+B outputs of one driver
   per motor, two TB6612 boards for the pair, or `AIN`/`BIN` tied together) → 2.4 A continuous /
   6.4 A peak. Use this if thermal tests (R-8) show the board above 70 °C.
5. **Upgrade path** if the robot is heavy or tests fail: DRV8874 / VNH-class drivers with current
   sense. The driver is isolated behind `motor_drv.h` in PRU1/M4F so swapping is a PRU1 pin mapping change.

Because normal balancing needs only a few hundred mA and peaks occur on pushes, TB6612 is
workable for a light (≤ 1.2 kg) robot. Budget for a heat sink and airflow.

## 8.3 Pin budget update (doc 1, 1.8)

| Signal | Count | Owner | Notes |
|---|---|---|---|
| Encoder L A/B, R A/B | 4 | PRU0 inputs | encoders powered from 3.3 V |
| Encoder power | 3.3 V + GND ×2 | — | from the PB2 3.3 V rail via small LC filter, or a separate LDO if current is tight (each encoder ≈ 10–20 mA) |
| Motor L `PWM`, `IN1`, `IN2` | 3 | PRU1 outputs | |
| Motor R `PWM`, `IN1`, `IN2` | 3 | PRU1 outputs | |
| `STBY` | 1 | hardware: E-stop switch + pull-down; also read back/controlled by M4F GPIO through a diode-OR | |

## 8.4 720p USB camera

Verified on your board with `v4l2-ctl --list-formats-ext`:

| Format | Modes |
|---|---|
| **MJPG** (compressed) | 1280x720 @ 30, 1920x1080 @ 30, 640x480 @ 30 |
| YUYV (raw) | 1280x720 @ 10, 1920x1080 @ 5, 640x480 @ 30 |

Consequences: always use **MJPG**. The camera has no 640x360 mode and no H.264, and MJPG
offers 30 fps only, so lower frame rates are produced by dropping frames in the server (this
saves network bandwidth, not USB bandwidth).

| Profile | Mode | Typical bitrate (MJPEG) | Use |
|---|---|---|---|
| FHD (optional) | 1920x1080 @ 30 MJPG | ~20–40 Mbit/s | recording/inspection only; heavy for WiFi and phone decode |
| **HQ (default V1)** | 1280x720 @ 30 MJPG | ~10–25 Mbit/s | 5 GHz LAN/AP, one client |
| Normal | 1280x720 MJPG, 15 fps sent | ~5–12 Mbit/s | several clients / moderate WiFi |
| Low | 640x480 @ 30 MJPG | ~3–8 Mbit/s | weak WiFi |
| Low-15 | 640x480 MJPG, 15 fps sent | ~2–4 Mbit/s | very weak WiFi, WebRTC source |

- **V1 (MJPEG passthrough)** needs no decode on the A53: it forwards the camera's JPEG frames.
- **WebRTC V2b with software x264 cannot use 720p** on the two A53s with headroom for WiFi and USB.
  Use the camera's 640x480 MJPG mode (cheap JPEG decode with libjpeg-turbo), then x264
  `ultrafast zerolatency` at ~1 Mbit/s. This is 4:3, while 720p is 16:9, so the UI must handle
  both aspect ratios. H.264 passthrough is **not available** with this camera.
- Camera mounting: rigid and low-vibration; rolling-shutter jelly shows up when the robot
  wobbles. Camera current ~200–300 mA at 5 V. Auto-exposure may reduce the frame rate in dim light
  (set exposure mode and `exposure_dynamic_framerate=0`).
- Latency target: V-1 (< 250 ms median) is judged at the *Normal* profile first; HQ has
  more USB and network buffering, so confirm it separately.
