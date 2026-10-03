# Robot software and hardware

Two-wheel self-balancing robot on PocketBeagle 2. Design documents: [`docs/balance-robot/`](../docs/balance-robot/README.md).

| Directory | State |
|---|---|
| [`core/`](core/) | **Portable C library, host-tested**: CRC, protocol, MPU-6500 helpers, estimators, speed estimation, PID, balance controller, state machine, PRU mailbox layout. 10 test programs, ASan/UBSan clean |
| [`sim/`](sim/) | **Closed-loop simulator**: nonlinear plant in Python driving the real C controller through `libbalbot.so` (SIM tests) |
| [`linux/balbotd/`](linux/balbotd/) | **Linux supervisor + fake M4F**: WebSocket/REST, lease arbitration, failsafe, rpmsg bridge; 99 tests |
| [`hardware/`](hardware/) | KiCad Rev 0 schematic, pin map, BOM, tests (see its README) |
| [`firmware-m4/`](firmware-m4/) | Zephyr M4F application **skeleton, not built** (no Zephyr SDK available yet; hardware access is stubbed) |
| [`firmware-pru/`](firmware-pru/) | PRU plan only; waits for the verified pin map |

Not started: web UI, React Native app, video service, BLE bridge.

## Run the tests

```sh
# C unit tests with sanitizers
cmake -S robot/core -B build -G Ninja -DSANITIZE=ON && cmake --build build && ctest --test-dir build --output-on-failure

# closed-loop simulation
pip install -r robot/sim/requirements.txt
cmake -S robot/core -B robot/core/build-shared -G Ninja -DBUILD_SHARED=ON && cmake --build robot/core/build-shared --target balbot
cd robot/sim && python3 -m pytest -q test_sim.py
```

## Conventions

Sign and unit conventions are in `core/include/balbot/conventions.h` (pitch > 0 = leaning forward; positive volts drive the
robot forward; yaw > 0 = left). Tests check them, so a wrong sign fails on the host rather than on the robot.

## What the simulator says (and does not)

The controller defaults are an LQR-derived cascade for the *example* robot in `sim/plant.py` (0.9 kg body, 65 mm wheels,
GB37-520 1:30 with assumed 5.5 ohm winding+driver resistance and 3e-6 kg m^2 motor rotor inertia). The motor resistance, rotor
inertia, mass and CG height are guesses until measured (doc 05, 5.1), so **the gains must be re-derived for the real robot**.
The simulation shows the structure and sign conventions are sound, not that your robot will balance with these numbers.
