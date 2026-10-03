# 6. Test plan, unit tests and CI

## 6.1 Strategy: a test pyramid that reaches the hardware last

```
            ┌────────────┐
            │ field/soak │  few, slow, manual + logged
           ┌┴────────────┴┐
           │ HIL on board │  nightly, self-hosted runner with a PocketBeagle 2 + rig
          ┌┴──────────────┴┐
          │ SIL / closed-  │  every PR: C controller + Python plant, hundreds of scenarios
          │ loop simulation│
         ┌┴────────────────┴┐
         │ unit + component │  every push: milliseconds each, host-native, > 90 % of the logic
         └──────────────────┘
```

Principle: **everything algorithmic is plain C in `robot/core/` and is exercised on the host**;
firmware, PRU code and servers are thin shells around tested logic. A bug found on the robot
costs minutes to hours; on the host costs seconds.

## 6.2 Test levels and tools

| Level | Scope | Tooling | Runs |
|---|---|---|---|
| UT-C | `core/` C library: filters, PID/LQR, state machine, protocol codec, calibration math | CMake + Unity (or CMocka) + gcov/lcov; sanitizers (ASan, UBSan) | every push |
| UT-DRV | MPU6500 driver, PRU mailbox logic against mocks | same, register-level SPI mock; Zephyr `ztest` on `native_sim` | every push |
| UT-Z | Zephyr app logic and threads | `west twister` on `native_sim` and QEMU Cortex-M4 (`mps2/an386` or similar, verify availability) | every PR |
| UT-PRU | quadrature table and PWM math | the PRU C source is split: pure functions (`qdec_step`, `duty_to_ticks`) compiled for host in UT-C; PRU-specific code built with `clpru` in CI but not executed | every push |
| UT-PY | `balbotd`, arbiter, protocol, REST/WS | `pytest`, `pytest-asyncio`, `httpx`, `websockets`; `hypothesis` for protocol fuzzing | every push |
| UT-JS | shared TS protocol package, web UI, RN app | Jest, React Native Testing Library, `tsc --noEmit`, ESLint | every push |
| SIM | closed-loop: C controller ↔ Python nonlinear plant | `pytest` + `ctypes`/`cffi`, `numpy`, `scipy`; seeds fixed | every PR |
| IT | `balbotd` ↔ **fake M4F** (rpmsg over a pty/socket), WS clients, BLE frames through a BlueZ mock | pytest, docker-compose (server + fake robot + test client) | every PR |
| E2E-APP | RN app against the simulator server | Maestro or Detox on emulators (nightly) | nightly |
| HIL | firmware on a real PB2 with the plant simulated (HIL-SIM) and with the real robot on a rig | self-hosted GitHub runner, pytest-based harness, USB/serial relay, power switch | nightly + before release |
| SOAK | 1 h balance, 24 h idle with video/WiFi | scripted | weekly/release |

## 6.3 Unit test catalogue (core, with representative cases)

### IMU driver and conversion (`UT-DRV-IMU`)

| ID | Test | Pass criteria |
|---|---|---|
| IMU-1 | init sequence | mock SPI log equals the golden sequence of 4.3 (address, value, order, delays, clock changes) |
| IMU-2 | WHO_AM_I mismatch (0x68, 0x71, 0x00, 0xFF) | returns `ERR_ID`, no config writes beyond reset |
| IMU-3 | burst parse | known 15-byte frames → known raw ints, including negatives and ±32768 |
| IMU-4 | unit conversion | ±1 LSB matches `1/8192 g`, `1/32.8 °/s` within float epsilon |
| IMU-5 | axis maps | all 24 orientations: rotated gravity vector maps to +z for each; determinant = +1 |
| IMU-6 | health checks | stuck data, all-FF, stale data → fault after 3 events, not after 2 |
| IMU-7 | `dt` measurement | jittered timestamps → filtered `dt` within 1 %, clamped to [0.5, 2] ms |
| IMU-8 | missed DRDY | gap > 1.5 ms increments counter, integration uses real gap |

### Calibration (`UT-CAL`)

| ID | Test | Pass |
|---|---|---|
| CAL-1 | gyro bias | synthetic constant bias + noise σ → estimated within 3σ/√N |
| CAL-2 | motion rejection | bump during window → restart, never accept a biased result |
| CAL-3 | 6-position solver | random offset/scale/misalignment → recovered to 0.1 % / 0.05° |
| CAL-4 | temp fit | linear drift data → slope within 5 % |
| CAL-5 | online bias | stationary 60 s with step bias → tracks within 10 s; no update when moving |

### Estimator (`UT-EST`)

| ID | Test | Pass |
|---|---|---|
| EST-1 | static tilt | converges to true pitch within 0.1° after 3τ |
| EST-2 | constant rate rotation | integrates 90° over 1 s within 0.5 % (gyro path) |
| EST-3 | gyro bias step | Kalman: bias converges; complementary: documented steady-state error |
| EST-4 | accel disturbance | 1 g-pulse for 100 ms → pitch error < 2° (gating works) |
| EST-5 | frequency response | sine tilt 0.2–10 Hz → gain/phase vs. analytic complementary filter response |
| EST-6 | numeric health | 10⁷ samples random input: no NaN/Inf, `P` positive definite |
| EST-7 | C vs. Python reference | agree within 1e-4 rad on all golden vectors (4.10) |
| EST-8 | quaternion norm | |q| = 1 ± 1e-6 after 10⁶ steps |
| EST-9 | speed estimator, fixed window | constant-speed count streams (1:30, 0.155 mm/count) at 0.02–1.1 m/s → error < 1 quantum |
| EST-10 | speed estimator, M/T | below 0.15 m/s error < 2 % and no 100 Hz staircase; counter wrap and direction reversal handled |

### Controller (`UT-CTL`)

| ID | Test | Pass |
|---|---|---|
| CTL-1 | sign convention | θ>0 ⇒ u>0; θ̇>0 ⇒ u>0; θ_ref>0 ⇒ u<0 (initial transient) |
| CTL-2 | PID math | step/ramp/steady inputs equal a hand-computed reference |
| CTL-3 | D on measurement | setpoint step produces no derivative spike |
| CTL-4 | anti-windup | long saturation: integrator bounded, recovers within N ms after release |
| CTL-5 | voltage normalisation | `V_bat` 9.0–12.6 V: duty scales exactly, clamps at ±0.95 |
| CTL-6 | dead-zone comp | continuous through zero, no chatter at |u|<ε |
| CTL-7 | limiters | rate limits, `θ_max`, `a_max`, headroom limiter obey bounds for random command sequences (property-based) |
| CTL-8 | mixing | turn term reduced before balance term when voltage is limited |
| CTL-9 | LQR | gains vs. `scipy.linalg.solve_discrete_are` reference within 1e-6 |
| CTL-10 | determinism | same inputs ⇒ identical outputs (no uninitialised state, no globals) under valgrind/MSan |

### State machine and safety (`UT-SM`)

| ID | Test | Pass |
|---|---|---|
| SM-1 | every legal transition | table-driven, one case per edge |
| SM-2 | every illegal transition | rejected, state unchanged, event logged |
| SM-3 | tip fault | |θ| > 35° for 1 sample → FALLEN, output exactly 0 |
| SM-4 | arm preconditions | arm refused unless |θ|<5° held 1 s, calibrated, battery ok, no fault |
| SM-5 | command timeout | no CMD for 500 ms ⇒ ramp to 0 then LAYING_DOWN at 10 s |
| SM-6 | estop | accepted in every state, latched, reset only via sequence |
| SM-7 | fuzz | random event sequences never reach a state with motors active outside BALANCING/LAYING_DOWN/RAISING |

### Protocol and codecs (`UT-PROTO`)

| ID | Test | Pass |
|---|---|---|
| PRO-1 | CRC16/CRC8 | standard check vectors (`"123456789"` → 0x29B1 for CCITT-FALSE) |
| PRO-2 | encode/decode round trip | all message types, boundary values |
| PRO-3 | malformed input | truncated, wrong len, bad CRC, unknown type, oversize → rejected, no crash (fuzz 10⁶ cases, `libFuzzer`/AFL++) |
| PRO-4 | sequence | wrap at 65535, duplicates, gaps detected |
| PRO-5 | cross-language | golden vectors in `shared/vectors/*.json` decode identically in C, Python, TypeScript |
| PRO-6 | BLE frames | 8-byte control frame pack/unpack, CRC8 |

### Server (`UT-PY`) and apps (`UT-JS`)

- Lease arbitration: first-come, expiry at 1 s, preemption order, estop from viewer, epoch
  mismatch dropped, transport switch rules (3.5).
- Failsafe: with a fake clock, no `drive` for 250 ms ⇒ zero command emitted; reconnect does not
  resume stale commands.
- Rate limiting and message-size caps; auth roles; REST schema validation (OpenAPI via
  Schemathesis).
- Telemetry fan-out and per-client decimation, back-pressure (slow client does not delay
  others).
- JS: protocol package, joystick-to-intent mapping (deadband/expo), reconnect state machine,
  screens snapshot tests, accessibility labels on E-STOP.

## 6.4 Closed-loop simulation tests (SIM)

The C controller is compiled as a shared library and driven by the Python nonlinear plant at
a fixed step (e.g. 0.1 ms plant, 1 ms/2 ms controller) including: IMU noise (σ from datasheet),
bias, quantisation, 2.9 ms filter delay, motor dead zone/backlash/inductance, battery sag,
encoder quantisation, PWM latency, and random seeds.

| ID | Scenario | Pass criteria |
|---|---|---|
| SIM-1 | stand from ±5° initial tilt | settles |θ|<0.5° within 2 s, no saturation > 200 ms |
| SIM-2 | impulse push (equivalent to 0.3–1.0 m/s velocity kick) | recovers; peak tilt < 15°; drift < 0.5 m |
| SIM-3 | estimator comparison (A vs B vs C) | metrics table; each meets SIM-1/2; chosen one best on RMS error |
| SIM-4 | speed step 0 → 0.6 m/s, 0.6 → 0 | rise < 1.5 s, overshoot < 20 %, tilt < θ_max |
| SIM-5 | steering step ±2 rad/s while moving | yaw rate error < 10 %, balance maintained |
| SIM-6 | parameter sweep ±30 % of m, l, I, M, Kt, R, friction | ≥ 95 % of 500 Monte-Carlo runs stable (robustness margin) |
| SIM-7 | delay sweep 0–12 ms | stability boundary documented; nominal has ≥ 3 ms margin |
| SIM-8 | sensor faults: gyro bias step, accel spike, stuck IMU | correct fault state or graceful recovery, never uncommanded motion |
| SIM-9 | battery 12.6 → 9.0 V (3S 18650) during run | no loss of stability, headroom limiter triggers |
| SIM-10 | slope ±8°, wheel slip | held or fails safe, documented limit |
| SIM-11 | link loss during motion | stops commanding speed within 250 ms, remains balanced, lies down at 10 s |
| SIM-12 | regression of gains | PR changing `params.yaml` or `core/` must keep SIM-1…11 green |

Results are written as JUnit XML plus plots (PNG) uploaded as CI artifacts for review.

## 6.5 Hardware-in-the-loop (HIL)

### HIL-SIM (no motors, no robot): on-target firmware vs. simulated plant

The real M4F image runs with `CONFIG_BALBOT_SIM_PLANT=y`: the IMU/encoder inputs come from a
simulated plant stepped on the M4F (or fed from the host over rpmsg) and PRU outputs are
read back. Verifies timing, scheduling, memory, and PRU/M4F integration with no mechanical
risk.

| ID | Test | Pass |
|---|---|---|
| HIL-1 | boot to `STANDBY` from cold | < 30 s; `fw` version matches; no fault |
| HIL-2 | loop timing | 10 min: IMU→PWM period 1.000 ± 0.020 ms, worst-case execution < 100 µs, zero overruns, jitter histogram stored |
| HIL-3 | PRU1 failsafe | stop incrementing heartbeat (debug command) → PWM brakes within 10 ms (logic analyser/scope) |
| HIL-4 | PRU0 counting | frequency generator drives A/B at 10 kHz–200 kHz edges, both directions → exact counts, zero illegal transitions |
| HIL-5 | rpmsg stress | 10 min at 1 kHz commands + 100 Hz telemetry: no loss of control path, drop counters reported |
| HIL-6 | A53 stall | `kill -STOP balbotd` / stress-ng CPU+IO load → M4F loop unaffected, command timeout path behaves |
| HIL-7 | M4F crash | trigger fault → PRU1 brakes; `balbotd` reports `M4F_DEAD`, restart via remoteproc works |
| HIL-8 | PRU firmware reload | reload without reboot, state recovers |
| HIL-9 | power fail | brown-out injection (programmable supply) → no spurious PWM on power-up (outputs in brake/high-Z) |

### HIL-BENCH: IMU on a tilt table / real sensor

A servo or stepper tilt rig sweeps known angles; compare the filtered pitch to an encoder on
the rig: static accuracy < 0.5°, dynamic tracking error < 1° RMS at 1 Hz, repeatability
across 100 sweeps, temperature drift characterisation.

### HIL-ROBOT: real robot on a safety rig (tethered, soft-landing mat, current-limited
supply or fused battery)

| ID | Test | Pass |
|---|---|---|
| R-1 | direction/sign checks (wheels, encoders, pitch) | each verified before first balance; recorded as checklist |
| R-2 | balance 60 s still | |θ| < 1° RMS after settle, wheel travel < 0.3 m |
| R-3 | push recovery | 10 pushes of the same impulse: ≥ 9 recover |
| R-4 | drive pattern | 2 m forward/back, figure-8 at 0.4 m/s with the app; max tilt < 12° |
| R-5 | link loss | pull WiFi/kill app mid-drive → stops within 0.5 s, stays balanced |
| R-6 | e-stop button | motors off < 20 ms (scope), robot falls onto bumpers without damage |
| R-7 | low battery | cutoff and lay-down behaviour, no brown-out reset of the board |
| R-8 | thermal | 30 min run: motor driver < 70 °C, SoC < 80 °C with video streaming |
| R-9 | video + WiFi load while balancing | no change in IMU loop jitter vs. idle (HIL-2 metric) |
| R-11 | TB6612 thermal/current | 30 min mixed pushes: driver < 70 °C, no thermal-shutdown, estimated vs. INA226 current within 25 %, stall detect trips in < 300 ms when a wheel is blocked |
| R-10 | tip/fall | fall from balance: motors brake, sensors/board undamaged, FALLEN latched |

## 6.6 Non-functional tests

| ID | Area | Test | Target |
|---|---|---|---|
| V-1 | video | glass-to-glass latency (LED on robot, filmed + screen capture or timestamp overlay) | median < 250 ms, p95 < 400 ms on 5 GHz LAN |
| V-2 | video | frame rate and drops 10 min | ≥ 28 fps at 640x480; < 1 % dropped |
| V-3 | video | A53 CPU use | < 30 % total during stream with 1 client, < 60 % with 3 |
| V-4 | video | WiFi degradation (tc netem 5 % loss, 100 ms jitter) | stream recovers, control unaffected |
| N-1 | control latency | command to wheel response (app tap → PWM change on scope) | median < 80 ms WiFi, < 150 ms BLE |
| N-2 | control robustness | 5 % loss + 50 ms jitter with netem | no unsafe behaviour; limiter and timeout work |
| N-3 | multi-client | 3 simultaneous clients, driver handover | arbitration rules hold |
| N-4 | security | no unauthenticated control, token brute force throttled, fuzzing of WS/REST/BLE inputs, TLS config scan | no findings above low |
| N-5 | BLE | iOS and Android devices, pairing, reconnect, background behaviour | works on a device matrix (3 phones each OS) |
| N-6 | OTA | M4F/PRU/balbotd update, interrupted power at each step | boots to last good image |
| N-7 | resilience | 24 h idle, 1 h soak balancing | no leaks (RSS stable), no faults, no reboots |
| N-8 | EMC/EMI | motor noise coupling into IMU/SPI | IMU noise RMS with motors at 50 % duty within 20 % of idle |

## 6.6b Status of the implementation (authoring branch)

Implemented and passing locally: `robot/core` (10 C test programs, ASan/UBSan, line coverage 97 %), `robot/sim`
(17 closed-loop tests), `robot/hardware` (18 tests), `robot/linux/balbotd` (lease, failsafe, limits, config round trips, lossy link, M4F death and reconnect, real-process end-to-end, Python-vs-C frame cross-check, video hub and MJPEG, 24 Node unit tests for the page logic, and Chromium end-to-end tests of the page itself). Workflows `robot-ci.yml` and `robot-hardware.yml` exist but have not
run on GitHub yet. Of the catalogue below, the unit tests for CRC, protocol (incl. fuzz-lite), IMU conversion/health/dt,
gyro calibration, estimators, speed estimation, PID, controller and state machine exist, as do SIM-1/2/4/5/6/7/8/9 in
simplified form. Not implemented: 6-position calibration, Mahony filter, LQR code, HIL, server and app tests.

## 6.7 CI design

Existing workflow: `.github/workflows/cross-compile.yml` (cross-builds the GamePup stack on
`push` to `main` and `cursor/**`, and on PRs). The robot adds **new workflows** so the slow
GamePup/N64 build does not gate the fast robot checks; they use path filters.

### Workflow `robot-ci.yml` (every push/PR touching `robot/**`, `docs/balance-robot/**`)

| Job | Runner | Steps | Time |
|---|---|---|---|
| `core-tests` | ubuntu-24.04 | cmake, build with `-Wall -Wextra -Werror`, run Unity tests, ASan/UBSan, gcov → lcov; fail if line coverage < 90 % on `core/`, branch < 80 % | ~2 min |
| `static-analysis` | ubuntu-24.04 | `clang-tidy`, `cppcheck --enable=all`, MISRA-subset (cppcheck addon) report on `core/`, `clang-format --dry-run` | ~3 min |
| `fuzz` | ubuntu-24.04 | libFuzzer 60 s on protocol decoder + SM fuzz, corpus cached | ~2 min |
| `sim` | ubuntu-24.04 | build core as shared lib, pytest SIM-1…12 (parallel with `pytest-xdist`), upload plots | ~10 min |
| `python` | ubuntu-24.04 | `ruff`, `mypy --strict`, `pytest --cov` (≥ 90 %), contract tests vs. fake M4F | ~3 min |
| `ts` | ubuntu-24.04 | `pnpm i --frozen-lockfile`, `tsc`, ESLint, Jest (protocol, web UI, RN logic) | ~4 min |
| `zephyr-native` | ubuntu-24.04 | `west init/update` (cached), `west twister -p native_sim -T robot/firmware-m4/tests`, coverage | ~8 min |
| `zephyr-build` | ubuntu-24.04 | Zephyr SDK container, `west build -b <pocketbeagle_2 m4 target>` for release+debug, size report, fail if flash/RAM > 85 % | ~8 min |
| `pru-build` | ubuntu-24.04 | TI `clpru` + PRU Software Support Package (cached download, version pinned and checksummed), build both firmwares, check `.map` memory use | ~3 min |
| `dtbo` | ubuntu-24.04 | `dtc -@` robot overlay, `fdtoverlay` merge test against the PB2 base dtb for the pinned kernel (reuses `ci/target.env`) | ~1 min |
| `hardware` | ubuntu-24.04 | **implemented** as `.github/workflows/robot-hardware.yml`: pin-map rules, schematic/BOM reproducibility, netlist checks, PDF artifact; KiCad 8 ERC advisory. Rev A adds `pcb drc`, Gerber/STEP and pin-map vs. DT overlay/firmware checks (doc 9.6) | ~3 min |
| `docs` | ubuntu-24.04 | markdown link check, spell check, diagram render | ~1 min |
| `package` | needs: all | produce `dist/robot/` (m4.elf, pru0.out, pru1.out, dtbo, balbotd wheel, web bundle) and upload artifact + `SHA256SUMS` | ~2 min |

Branch protection on `main`: required checks `core-tests`, `static-analysis`, `sim`, `python`,
`ts`, `zephyr-native`, `zephyr-build`, `pru-build`.

### Workflow `robot-nightly.yml`

| Job | Runner | Content |
|---|---|---|
| `sim-montecarlo` | ubuntu | 5 000-run robustness sweep (SIM-6/7), trend stored as artifact |
| `fuzz-long` | ubuntu | 30 min per fuzzer |
| `rn-e2e` | macos-latest + ubuntu (emulators) | RN builds (Android debug APK, iOS simulator build), Maestro flows against the fake robot server |
| `hil` | **self-hosted** `[self-hosted, pb2, hil]` | flash latest artifact, run HIL-1…9 and V-1…V-4 (when camera/WiFi attached), upload logs, jitter histograms, scope captures |

### HIL runner design

Raspberry Pi/PC host next to the PB2 + rig: USB serial to the M4F shell/Linux console,
relay board (power cycle, E-stop), programmable supply, logic analyser (Saleae Logic 8 via
`sigrok` CLI or a PRU-based self-capture), signal generator or second microcontroller for
encoder emulation, tilt table. Harness in pytest with fixtures `board`, `power`, `scope`,
`robot_api`. Runner label guarded to the protected branches only (no PR from forks) because
it can drive motors. A hardware **watchdog relay** cuts motor power if the HIL host stops
polling.

### CI conventions

- Cached: Zephyr workspace/toolchain, pnpm store, ccache, TI compiler tarball.
- All third-party downloads pinned by version + SHA256 (as `ci/target.env` does for the board image).
- Every job uploads logs; failures print the minimal reproduction command.
- Matrix: Python 3.11/3.12; Node LTS; `gcc-14` and `clang` for `core`.
- Releases are tagged `robot-vX.Y.Z`; release notes are generated from conventional commits and
  include the tested HIL result summary.

### Sketch of `robot-ci.yml`

```yaml
name: Robot CI
on:
  push: {branches: [main, 'docs/**', 'robot/**'], paths: ['robot/**', 'docs/balance-robot/**', '.github/workflows/robot-*.yml']}
  pull_request: {paths: ['robot/**', 'docs/balance-robot/**']}
concurrency: {group: robot-${{ github.ref }}, cancel-in-progress: true}
jobs:
  core-tests:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
      - run: sudo apt-get update && sudo apt-get install -y cmake ninja-build lcov clang gcc-14
      - run: cmake -S robot/core -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCOVERAGE=ON -DSANITIZE=ON
      - run: cmake --build build && ctest --test-dir build --output-on-failure
      - run: |
          lcov --capture --directory build --output-file cov.info
          lcov --summary cov.info | tee cov.txt
          python3 robot/tools/check_coverage.py cov.txt --lines 90 --branches 80
      - uses: actions/upload-artifact@v4
        with: {name: core-coverage, path: cov.info}
  sim:
    needs: core-tests
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-python@v5
        with: {python-version: '3.12', cache: pip}
      - run: pip install -r robot/sim/requirements.txt
      - run: cmake -S robot/core -B build -DBUILD_SHARED=ON && cmake --build build
      - run: pytest robot/sim -n auto --junitxml=sim.xml
      - uses: actions/upload-artifact@v4
        if: always()
        with:
          name: sim-results
          path: |
            sim.xml
            robot/sim/out/**
```

## 6.8 Quality gates and metrics

| Metric | Gate |
|---|---|
| `core/` line / branch coverage | ≥ 90 % / ≥ 80 % |
| Zephyr/ Python / TS coverage | ≥ 80 % / ≥ 90 % / ≥ 80 % |
| Static analysis | zero high-severity; MISRA-subset deviations documented |
| Loop WCET on target | < 100 µs (HIL-2), regression > 20 % blocks merge |
| Sim stability margin | SIM-6 ≥ 95 %, SIM-7 margin ≥ 3 ms |
| Firmware memory | M4F RAM/flash < 85 % of available |
| Flaky tests | quarantine is **not** allowed for safety tests; a flaky test is a bug |
| Reproducibility | firmware builds bit-identical for the same commit (pinned toolchains) |

## 6.9 Test data and traceability

- Requirements are numbered (`REQ-BAL-01` …) in `docs/balance-robot/requirements.md` (created
  in M0) and each test cites the requirement(s) it covers; CI fails if a requirement has
  no test reference (script `tools/trace_check.py`).
- Recorded blackbox logs from real runs are stored in `robot/tests/data/` (small, compressed)
  and replayed through estimator/controller as regression tests.
- Every field incident (fall) yields a new regression log + sim scenario before the fix.
