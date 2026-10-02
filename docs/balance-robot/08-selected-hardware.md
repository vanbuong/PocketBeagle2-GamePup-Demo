# 8. Selected hardware: JGB37-520 + TB6612 + 720p USB camera

Chosen parts: **JGB37-520 gearmotors with quadrature (Hall) encoders**, **TB6612FNG dual
driver**, **720p UVC USB camera**. This page records what that choice implies for the rest
of the design. Datasheet figures below are from memory of the TB6612FNG datasheet and
typical JGB37-520 listings; seller data varies, so **read your motor label and measure**
(procedure in doc 5, 5.1).

## 8.1 GB37-520 (JGB37-520) gearmotor with encoder: supplied specification

| Item | Value (from your spec sheet) | Consequence |
|---|---|---|
| Type | GB37-520 DC geared motor, 37 mm, 6 mm shaft, gearbox length 22 mm | |
| Voltage | 12 V DC | 3S 18650 (9.0–12.6 V) is the chosen supply |
| Ratio | **30:1** | confirmed; recommended ratio from the earlier analysis |
| No-load speed | **333 rpm** (34.9 rad/s) | wheel 65 mm: 1.13 m/s no-load |
| Speed under load | up to 250 rpm | wheel 65 mm: ≈ 0.85 m/s; plan `v_max` ≈ 0.6 m/s |
| No-load current | 120 mA | friction/dead-zone indicator; start voltage to be measured |
| Max load current | **1 A** | within TB6612 1.2 A continuous per channel |
| Rated torque | 3.5 kg·cm (0.34 N·m) | |
| Max torque | 5 kg·cm (0.49 N·m) | |
| Encoder | Hall, 2 channels A/B, 3.3–5 V | power from 3.3 V, direct to PRU0 pins |
| Pulses | 11 per channel per motor-shaft rev (22 with both channels) → **330 per channel per output rev**, **1320 counts/rev at 4x decoding** | 0.155 mm per count with a 65 mm wheel |
| Not given | **stall current, winding resistance, start voltage** | must be measured before using the model (doc 5.1) |

### Model constants derived from the sheet (initial values; verify by measurement)

The output-shaft back-EMF constant is `Ke ≈ (12 V − I₀·R) / ω₀`. With `I₀ = 0.12 A` and `R` of a few ohms the
correction is ~0.3–0.5 V, so `Ke ≈ 0.33 V·s/rad` and, in SI units, `Kt ≈ Ke ≈ 0.33 N·m/A`. This agrees with the
sheet: 1 A × 0.33 N·m/A ≈ 0.34 N·m = the rated 3.5 kg·cm. The sheet's 5 kg·cm max torque
corresponds to ≈ 1.5 A, which suggests a winding resistance near 8 Ω (12 V / 1.5 A) *if* the
"max load" numbers are the stall point. A seller's stall current is often higher; **measure R**
(low-voltage stall test with a current-limited supply, wheel blocked, 1 s only) and put it in `config/robot-*.yaml`.
Add the TB6612 on-resistance (~0.5 Ω) to `R`.

### Sizing check (is the motor strong enough?)

Take a 1.2 kg robot, 65 mm wheels. Two motors at the rated torque give 2 × 0.34 / 0.0325 ≈ 21 N of
thrust. A 10° tilt recovery needs `m g tan 10° ≈ 2 N`, a 10° slope needs `m g sin 10° ≈ 2 N`, and
the 0.8 m/s² acceleration limit needs ≈ 1 N. So torque and current are ample (< 0.2 A in
normal balance); **the real limits are back-EMF voltage headroom at speed and the dead zone**
(doc 5.6 headroom limiter). Motor torque does not restrict the design until the robot is
heavier than ~3 kg.

### Encoder resolution and speed estimation

Confirmed 1:30, 65 mm wheel: 1320 counts/rev → **0.155 mm per count**.
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
| Current per channel | **1.2 A continuous, 3.2 A peak** (short pulses) | above the motor's 1 A max load current; stall current unknown, see mitigations |
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

With the supplied spec (max load current 1 A per motor, normal balancing a few hundred mA) the TB6612 is
adequate. The remaining exposure is an unspecified **stall current** (blocked wheel, fall onto the
wheels), covered by the stall detect and current estimate above. Budget a heat sink and airflow.

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
