# 5. Balance control, PID and motion

## 5.1 Plant model (wheeled inverted pendulum)

Symbols (SI): `m` body mass, `l` axle-to-CG distance, `I` body inertia about its CG (pitch),
`M` equivalent wheel mass (both wheels + rotor/gear inertia reflected to the wheel,
`M = 2·m_w + 2·J_w/r²`), `r` wheel radius, `g` = 9.81, `θ` pitch from upright (rad),
`x` position along the ground. Motor: torque constant `Kt`, back-EMF constant `Ke`
(`Kt ≈ Ke` in SI), winding resistance `R`, gear ratio folded into `Kt, Ke`.
Input `u` = voltage applied to both motors (average), `V_bat` the supply.

Linearised about θ = 0 (small angles, ignoring friction):

```
(M+m) ẍ + m l θ̈ = F
(I+m l²) θ̈ + m l ẍ = m g l θ
F = b·u − c·ẋ ,   b = 2 Kt /(R r) ,   c = 2 Kt Ke /(R r²)
```

Solving, with `Δ = (M+m)(I+m l²) − (m l)²`:

```
ẍ = [ (I+m l²)(b u − c ẋ) − m² g l² θ ] / Δ
θ̈ = [ m g l (M+m) θ − m l (b u − c ẋ) ] / Δ
```

State `s = [x, ẋ, θ, θ̇]ᵀ`:

```
      ⎡0   1                 0                  0⎤        ⎡ 0                 ⎤
A =   ⎢0  −(I+ml²)c/Δ    −m²gl²/Δ              0⎥ ,  B = ⎢ (I+ml²) b/Δ       ⎥
      ⎢0   0                 0                  1⎥        ⎢ 0                 ⎥
      ⎣0   m l c/Δ       m g l (M+m)/Δ          0⎦        ⎣ −m l b/Δ          ⎦
```

The open-loop pole at `p = +√( m g l (M+m)/Δ )` (≈ 5–8 rad/s for a 25–35 cm robot, i.e.
a fall time constant of ~150 ms) tells the required bandwidth: **loop rate ≥ 10 × p · (several)
→ 500 Hz is comfortable, 100 Hz is the practical minimum.** It also shows the motor must be
able to produce enough acceleration: required `ẍ_max ≈ g·tan θ_recover`.

`sim/plant.py` implements this linear model and the full nonlinear one
(`cos θ`, `sin θ`, `θ̇²` terms, Coulomb friction, gear backlash, voltage saturation, motor
inductance optional), parameterised from a YAML file with the measured robot values.

### Parameter identification (M4)

| Quantity | Method |
|---|---|
| `m`, `M` | scale |
| `l`, `I` | hang the body from the axle and measure the swing period `T`: `I_pivot = m g l T²/(4π²)` (small angle); `I = I_pivot − m l²` |
| `Kt`, `Ke`, `R` | JGB37-520 label/listing values then verified (add the TB6612 ≈ 0.5 Ω to `R`): `R` from stall current at low voltage, `Ke` from no-load speed at known V (`Ke = (V − I R)/ω`), `Kt = Ke` |
| friction / dead-zone | duty at which the wheel just starts to turn (each direction) |
| encoder CPR | count over exactly N wheel turns |
| delay | step response of the IMU→PWM chain measured on a scope with the PRU toggling a debug pin |

## 5.2 Control structure: cascaded loops

```
          v_cmd ──►(rate limit)──► Speed PI ──► θ_ref ─┐
 (wheel speed v) ─────────────────►┘ (outer, 100 Hz)    ▼
                                         Angle PD(+I) (inner, 500 Hz) ──► u_bal ──┐
     θ (filter), θ̇ (gyro) ────────────────────────────►┘                          ├─► L/R mix ─► V→duty ─► PRU1
 w_cmd ──►(rate limit)──► Yaw PI (100 Hz, uses ψ̇ gyro) ───────────────► u_turn ──┘
```

Inner loop gives the balance (fast, stabilising the unstable pole). The outer loop converts
a velocity request into a lean angle: leaning forward makes the base accelerate forward to
get back under the CG. The yaw loop adds an antisymmetric term to the two motors.

## 5.3 Inner loop: angle controller

```
e_θ   = θ − θ_ref − θ_trim                       // rad, positive when leaning forward of the target
u_bal = Kp·e_θ + Ki·∫e_θ dt + Kd·θ̇              // D on measurement (gyro), no derivative kick
```

Forward lean (θ > target) must drive the wheels **forward** (positive `u`) to get the base
back under the CG, matching `B` in 5.1 where positive `u` reduces θ. A *positive* `θ_ref` therefore
produces a backward wheel command first, which pitches the body forward, after which the
wheels follow it: this is the non-minimum-phase behaviour mentioned in 5.4.

- `θ̇` comes straight from the corrected gyro, not from differentiating θ: no extra
  noise, minimal lag (a hand-differentiated angle adds ≥ 1 sample delay and noise ×1000).
- `Ki` small or zero at first. Its job is rejecting a constant torque disturbance and a
  residual `θ_trim` error; large `Ki` destabilises an unstable plant. Typical: `Ki ≈ 0.02–0.1 Kp`.
- **Sign convention** (documented once in `core/conventions.h`): forward lean is positive
  pitch; positive `u` drives the wheels forward. A unit test checks that a positive pitch
  step yields positive `u` (a wrong sign is the most common first-power-up failure).
- **Anti-windup:** clamp the integrator so `|Ki·∫| ≤ 0.3·u_max`; conditional integration
  (freeze when `u` saturates and `e` has the same sign).
- **Output**: `u_bal` is a *voltage demand* (volts): `duty = u / V_bat_measured` so gains
  stay valid as the battery sags (voltage normalisation, see 5.8).
- **Dead-zone compensation**: add `sign(u)·u_dz` for `|u| > ε` (u_dz = measured start voltage,
  often 0.5–1.5 V), smoothed within ±0.1 V to avoid chatter around zero.
- **Output limits**: `|u| ≤ 0.9·V_bat`, rate limit ≈ 400 V/s *(initial)* to protect the driver.

### Why PD dominates

For `θ̈ ≈ a θ − k u` (a = p² ≈ 40–60 s⁻²), a pure P loop gives an undamped oscillator
(`θ̈ = (a − k Kp) θ`), so **`Kd` is what provides damping**; `Kp` must exceed `a/k` (to beat
gravity) and the closed-loop frequency `ω_n = √(k Kp − a)`, damping `ζ = k Kd/(2 ω_n)`. These
formulas give the *initial gains* offline from the identified model, then robot tuning
refines them.

## 5.4 Outer loop: speed to lean angle

```
e_v   = v_cmd_limited − v_meas                  // m/s
θ_ref = clamp( Kvp·e_v + Kvi·∫e_v dt , ±θ_max ) // θ_max ≈ 8–12°
```

- Sign: if the robot is slower than the command (`e_v > 0`) it must lean **forward**, so
  the base accelerates. Because the plant is non-minimum-phase near zero (the base first
  moves backward to lean forward), the outer loop is ≥ 5–10× slower than the inner (≈ 100 Hz
  update, bandwidth ≈ 1–2 Hz).
- `Kvi` also provides the slow *auto-trim*: when stationary and drifting, the integral
  settles at the lean angle that cancels CG offset; its value is copied slowly (< 0.05 °/s)
  into `θ_trim` and persisted.
- `v_meas` = filtered mean wheel speed from PRU0 (4.8). The speed loop is disabled
  (`θ_ref = 0`) if an encoder fault is flagged; the robot then balances but drifts.
- **Position hold** (optional): when `v_cmd = 0` for > 0.5 s, enable
  `v_target = −Kpos·(x − x_hold)` (saturated at 0.1 m/s) to stop drifting.

## 5.5 Steering (yaw) loop

```
w_meas  = ψ̇ (gyro z, optionally blended with encoders)
e_w     = w_cmd_limited − w_meas
u_turn  = Kyp·e_w + Kyi·∫e_w dt         // volts, clamped to ±0.3·V_bat
u_L = u_bal − u_turn ,  u_R = u_bal + u_turn          // (sign per mounting, tested)
```

Rate limits keep steering from consuming all the voltage headroom the balance loop needs:
`u_turn` is scaled down so that `max(|u_L|,|u_R|) ≤ 0.95·V_bat` **while preserving `u_bal`
priority** (turning is reduced first). In place turning moves left/right wheels in opposite
directions around a centre that must not move forward: `v_cmd` is not affected because the
speed estimate is the L/R *mean*.

## 5.6 Movement commands (how the device is driven)

| Input | Result |
|---|---|
| `vx ∈ [−1,1]` | `v_cmd = vx·v_max`, rate limited to `a_max` = 0.8 m/s² (initial). Acceleration limit is what bounds the pitch demand: `θ_acc ≈ atan(a/g)` ≈ 4.7° plus loop transients. |
| `wz ∈ [−1,1]` | `w_cmd = wz·w_max`, rate limited to 6 rad/s² |
| stop (`vx=0`) | decelerate with the same limit; position hold afterwards |
| `boost` flag | `v_max ×1.6`, `a_max ×1.5` (still clamped by `θ_max` and by motor voltage headroom) |
| speed vs. battery | `v_max` is scaled by `V_bat/V_nom` so low battery leaves headroom for recovery |
| ramps/gains | gain schedule: when |`v`| is high, `Kd` raised 10–20 % (initial) as dynamics change |

Because lean angle is limited to `θ_max` (≈ 10°) the *maximum sustainable speed* is bounded by
the motor back-EMF: the controller reduces `v_cmd` when `u_bal` is above 80 % of available
voltage (**headroom limiter**), otherwise the robot would run out of authority and fall
forward. This is a major real-world failure mode and has dedicated sim and HIL tests.

## 5.7 State machine

| State | Motors | Entry condition | Exit |
|---|---|---|---|
| `BOOT` | brake | power-up | IMU OK → `CALIBRATING` |
| `CALIBRATING` | brake | gyro bias | done → `STANDBY`; fail → `FAULT` |
| `STANDBY` | coast | calibrated | `ARM` + upright check → `BALANCING` |
| `RAISING` (optional, M9) | active | arm while lying on skids | reaches ±3° → `BALANCING` |
| `BALANCING` | active | \|θ\| < 5° for 1 s and arm command | tip/fault/disarm/low battery |
| `LAYING_DOWN` | active, scripted | command timeout, low battery | θ > 70° on skids → `STANDBY` |
| `FALLEN` | brake | \|θ\| > 35° (initial) | \|θ\| < 5° for 2 s and `DISARM→ARM` → `STANDBY` |
| `FAULT` | brake | IMU, encoder, overrun, estop, PRU failsafe | explicit reset when cause cleared |

Transitions are table-driven in `core/state_machine.c` with a **host test per edge**
(including illegal transitions). All safety transitions (→ `FALLEN`/`FAULT`) are evaluated
before the control calculation each tick; control output is forced to zero in those states.

Stand-up: the first version has no self-righting (operator lifts the robot). `RAISING`
(a "kick-up" from a skid or tail using a scripted torque profile and a handover to the
balance loop at ~±3°) is a stretch goal.

## 5.8 Voltage and timing details

- Measure `V_bat` at 10–100 Hz, low-pass 5 Hz; `duty = clamp(u / V_bat_f, −0.95, 0.95)`.
- Delay compensation: total loop delay ≈ gyro DLPF 2.9 ms + 1 ms (sample) + ~1 ms
  (control quantisation) + 0.05 ms (PWM) + motor electrical ≈ 5 ms. With a 150 ms unstable
  time constant this delay consumes much of the phase margin → *the main reason `Kd` cannot be
  raised arbitrarily*. Reducing the DLPF to 250 Hz/8 kHz sampling mode and averaging in
  firmware is an experiment (E2) if needed.
- PWM at 20 kHz (above audible), slew limit on duty prevents shoot-through / current spikes.
- Motor current limit: TB6612 has no current sense and is rated 1.2 A continuous, so use the
  firmware current estimate, stall detection and battery-rail INA226 of doc 8.2; fault when
  stalled above limit > 200 ms.

## 5.9 Tuning procedure (robot on the ground, hand-supported, tether, wheels free first)

Preconditions: sign tests pass (wheel forward direction, encoder counting direction,
pitch sign), `θ_trim` approx., battery ≥ 80 %.

1. Wheels off the ground. Inner loop only (`Kvp=Kvi=0`, yaw loop off). `Ki = 0`.
   Tilt by hand: wheels should spin in the lean direction. Check proportional response and
   the dead-zone offset.
2. Set `Kd` to 20 % of the model value and `Kp` to model value; ground contact with a
   hand/guard rail. Increase `Kp` until the robot stands with slow oscillation (±3°); then
   raise `Kd` until the oscillation is damped (high-frequency buzzing means `Kd` is too high
   or the gyro filter is too slow).
3. Add `Ki` slowly to remove the steady lean drift; tune `θ_trim`.
4. Enable the speed loop, `Kvi = 0`. Raise `Kvp` until the robot stops drifting when
   pushed but does not oscillate at ~1 Hz (that "see-saw" is the outer loop interacting with
   the non-minimum-phase response). Add `Kvi` for steady speed.
5. Enable yaw `Kyp/Kyi`.
6. Disturbance tests (doc 6, HIL-3): push impulse, kerb, ramp, battery drop.
7. Record gains in `config/robot-<name>.yaml`; the tuning page writes them and `CFG_SAVE`
   persists them. Keep a "known-good" slot.

Typical starting values (formulated for a 0.8–1.2 kg, 25–30 cm robot with 65 mm wheels and
330 rpm 12 V gearmotors, to be recomputed from the model): `Kp ≈ 20–35 V/rad`, `Kd ≈ 0.8–2 V·s/rad`,
`Ki ≈ 0–2 V/(rad·s)`, `Kvp ≈ 0.1–0.2 rad/(m/s)`, `Kvi ≈ 0.02–0.05`, `Kyp ≈ 0.5–1 V·s/rad`.
*(These are order-of-magnitude folklore and unverified for this build.)*

## 5.10 LQR alternative (milestone M5)

Using `A, B` from 5.1 (discretised at 500 Hz, ZOH, with a delay state or a Padé
approximation of the 5 ms delay), compute

```
K = dlqr(A_d, B_d, Q, R),   Q = diag(q_x, q_v, q_θ, q_ω),  R = r
u = −K (s − s_ref),   s_ref = [x_ref, v_ref, θ_trim, 0]
```

Typical weights start at `Q = diag(1, 1, 100, 1)`, `R = 1` scaled by volt units and tuned by
simulation and the "Bryson" rule (1/max²). Advantages: systematic multi-state coordination
(position, speed, angle, rate), natural gain scheduling by battery voltage, easy to add an
LQI integrator. Disadvantages: needs a good model, gains less intuitive on the bench.
Implementation is a 4-element dot product plus an optional integrator; the gains are
generated by `tools/lqr_design.py` into a header, which also makes the design reproducible
and tested. Both controllers share the same safety/state machine and limit logic; a
runtime flag selects PID or LQR, so they can be compared in the same HIL scenarios.

## 5.11 Fixed-point/float notes

Everything runs in `float32` on the M4F FPU; avoid `double` (software emulation, 10–50× slower),
avoid `atan2f`/`sinf` per sample where possible (a 3-term polynomial for small angles is
used for `atan2` in the fast path and checked against libm in unit tests with 0.05° error
tolerance). Constants live in a single `params.h` generated from YAML so tuning cannot
silently drift between C, Python and docs.
