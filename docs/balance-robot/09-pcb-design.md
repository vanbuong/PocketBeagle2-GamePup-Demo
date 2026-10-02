# 9. PCB and module design

Goal: a single **baseboard** that carries the PocketBeagle 2, power, motor driver, IMU and
the safety circuits, with off-the-shelf modules only where that reduces risk. Hardware
status: **design proposal, nothing fabricated**. Part numbers marked *e.g.* are examples to
check against current datasheets and availability before ordering; none were verified from
this environment.

## 9.1 Revision strategy (do not fabricate first)

| Rev | What | Purpose | Gate to next |
|---|---|---|---|
| **Rev 0** | breadboard/perfboard with modules (9.2), jumper wires, PB2 on a header board | validate pin muxing (PRU, MCU SPI), rpmsg, TB6612 current, IMU noise, USB/WiFi/camera on kernel 6.18-k3 | M0–M5 exit criteria met (doc 7); pin table frozen |
| **Rev A** | baseboard PCB, modules still used for USB hub and IMU | first integrated robot; includes all safety circuits | R-1…R-11 pass; no blue wires |
| **Rev B** | integrate USB hub IC, INA226 + shunt, optional 2nd TB6612 populated | production-like, smaller | field tests, thermal margins |

Reason: whether the needed PRU/MCU-domain pins reach the PB2 headers is **unconfirmed**
(risk R1, doc 7). A PCB routed before that is checked could be scrap. The schematic can be
started in parallel, but net-to-pin assignment is filled in after M0.

## 9.2 Modules to add (shopping list for Rev 0, and what becomes on-board in Rev A)

| # | Module / part | Needed? | Rev 0 (buy) | Rev A (on baseboard) | Notes |
|---|---|---|---|---|---|
| 1 | **IMU MPU-6500, SPI breakout** | required | GY-6500-style breakout that exposes NCS, SCLK, SDI, SDO, INT | 2×(1×8) header for the module, footprint placed at the board centre | many cheap boards are I2C-only or tie pins; check. MPU-6500 is an older part, so keep the footprint/header swappable (e.g. for a newer IMU) |
| 2 | **TB6612FNG driver** | have | your breakout | on-board TB6612FNG ×1, **second footprint (DNP)** to parallel channels | see doc 8.2 |
| 3 | **3S 18650 pack** | required | 3 cells in series (3S1P) with **3S BMS**, 10 A class, spot-welded or a quality holder | XT30 power connector + JST-XH balance/BMS lead | use matched, new, high-drain cells (≥ 10 A rating, e.g. Samsung 25R / Sony VTC / Molicel class); no mixed salvage cells; a charger for 3S Li-ion (12.6 V CC/CV or balance charger) outside the robot |
| 4 | **Fuse + main switch + reverse-polarity protection** | required | 5 A blade/glass fuse, rocker switch, P-MOSFET ideal-diode module | on-board 5–7.5 A fuse holder, P-FET reverse protection (*e.g.* low-Rds P-FET, ≥ 30 V), power switch header | sized for TB6612 peak 3.2 A ×2 plus logic |
| 5 | **5 V synchronous buck, ≥ 4 A** | required | *e.g.* Pololu D24V50F5-class 5 V/5 A module, or similar | on-board buck IC (*e.g.* TI TPS54xxx / TPS62xxx family, input range ≥ 14 V) | feeds PB2, USB hub/camera/dongle. Not LM2596 modules (noisy, inefficient). Keep switch node away from IMU |
| 6 | **3.3 V low-noise LDO** for sensors/encoders | recommended | *e.g.* AP2112K/TLV755-class 3.3 V ≥ 300 mA breakout | on-board LDO fed from 5 V | encoders (≈ 2×20 mA), IMU, INA; separate from the PB2 3.3 V pin whose current limit is unknown |
| 7 | **Battery current/voltage monitor** | recommended | INA260 breakout (integrated shunt, ±15 A, I2C) or INA226 + 10 mΩ shunt | INA226 + 10 mΩ 1 W Kelvin shunt (full scale 81.9 mV → ±8 A) | battery rail on the high side; address strap; must be on the I2C bus owned by the M4F, else values arrive over rpmsg at 10 Hz (doc 1) |
| 8 | **E-stop / arm hardware** | required | NC mushroom or latching button + arm button | footprints and 2-pin connectors | gating of `TB6612 STBY` (9.3) |
| 9 | **Hardware watchdog** (optional but recommended) | recommended | skip in Rev 0 | window/standard watchdog IC (*e.g.* STWD100 / TPS3431-class) on the STBY path, kicked by a PRU1 toggle | protects against both PRU1 and M4F hangs; see 9.3 |
| 10 | **USB 2.0 hub, powered, ≥ 3 ports** | required | an external powered hub | Rev A: USB-A sockets + header only; Rev B: hub IC (*e.g.* CH334/FE1.1s/USB2514B-class) with per-port current-limit switches (*e.g.* TPS2051 class) | 5 V for ports comes from the buck, not from PB2. How USB host reaches the headers on PB2 must be verified (the repo's GamePup cape provides a host port) |
| 11 | **WiFi + BT dongle** | required | MT7921AU-class USB adapter (in-kernel drivers, 5 GHz) | USB-A socket | doc 2.5 |
| 12 | **Camera 720p** | have | your UVC camera | USB-A or 4-pin JST-GH into the hub | MJPG 1280x720@30 verified (doc 8.4) |
| 13 | **Status UI** | optional | ILI9341 2.8" SPI display, SH1106 OLED, piezo buzzer, 3 LEDs | headers wired to the **same pins the GamePup overlays use** (SPI0 P2.25/27/29/31, D/C P2.17, reset P2.19, backlight P2.01) so `overlays/k3-am62-pocketbeagle2-spi0-ili9341.dts` works unchanged | shows IP, battery, BLE passkey (doc 3.4) |
| 14 | **UART debug header** | required | USB-UART adapter | 3.3 V 3-pin header for the M4F console, plus A53 debug UART | needed for bring-up of Zephyr on the M4F |
| 15 | **Encoder connectors / filters** | required | 6-pin breakout per motor | 6-pin JST connector per motor (verify your motor's connector) + RC filters | 9.4 |
| 16 | **Motor noise suppression** | required | 100 nF ceramic across each motor, 100 nF from each terminal to case | footprints at motor connector | reduces EMI into encoders/IMU |
| 17 | **Bulk capacitor + TVS on VM** | required | 470–1000 µF/25 V low-ESR electrolytic (or polymer) + TVS | on-board | TB6612 VM max 13.5 V and a full 3S pack is 12.6 V (doc 8.2) |
| 18 | **Optional sensors** | later | VL53L1X ToF, wheel-lift detection | I2C header | obstacle stop, lift-off detection |

## 9.3 Power and safety architecture

```
 3S 18650 ──BMS──fuse──reverse-protect──power switch──┬─► VM (TB6612 ×1–2) ─► motors
 (9.0–12.6 V)                                          │      ▲ bulk 470–1000 µF + TVS + 100 nF per driver
                                                       ├─► INA226/INA260 (high-side current + bus V)
                                                       ├─► 5 V buck ─► PB2, USB hub/ports (current-limited)
                                                       │              └─► 3.3 V LDO ─► IMU, encoders, INA, pull-ups
                                                       └─► (optional) E-stop sense

 STBY chain:  PRU1 "enable" ─┐
              M4F GPIO       ├─ AND ─► TB6612 STBY (10 k pull-down)
              E-stop NC ─────┘   ▲
              watchdog IC output─┘ (WDI toggled by PRU1; trips if no toggle for ~50 ms)
```

Rules:

- **Defaults off:** every PRU1 output to the TB6612 (`PWM`, `IN1`, `IN2`) has a 10 kΩ pull-down and
  `STBY` a 10 kΩ pull-down, because PB2 pins float during boot. The motors cannot move until
  firmware drives all three signals and `STBY` is released by all four conditions above.
- **E-stop is hardware:** it opens the gate regardless of firmware; the M4F also reads it.
- **BMS is the last resort, not the low-battery strategy.** A 3S BMS cuts at ~2.5 V/cell
  (~7.5 V pack), which would kill power mid-balance. Firmware lies the robot down at a
  filtered pack voltage of 3.3 V/cell (9.9 V) *(initial)* and disarms at 3.2 V; the 5 V buck must be
  chosen to run down to ≤ 8 V so the PB2 stays alive until then.
- **Regeneration and BMS disconnect:** braking pushes energy into VM; if the BMS opens
  (over-current/over-voltage) the supply disappears and VM can spike. Bulk capacitor + TVS
  clamp; keep BMS trip current well above 2× TB6612 peak (a 10–20 A BMS) and test (HIL R-6, R-7).
- **TB6612 VM limit:** a freshly charged 3S pack is 12.6 V against a 13.5 V rating, so no
  hot-plugging the battery (inrush ringing), no spikes above 13.5 V; verify on a scope.
  Alternative if tests show spikes: a 10 V motor rail from a buck converter (loses ~17 % top
  speed, gains supply independence from battery sag and spikes).
- **Pack life:** 3S1P 3000 mAh ≈ 33 Wh; estimated load 10–15 W average → ~2 h *(estimate; measure)*.
- **Grounds:** battery negative is the star point. Motor return current must not flow through
  the IMU/3.3 V analogue area: separate high-current ground pour from the logic ground and
  join under the TB6612/battery connector.

## 9.4 Signal-level details

| Signal group | Circuit |
|---|---|
| Encoder A/B (×4) | 3.3 V supply from the sensor LDO; 4.7 kΩ pull-up footprint (DNP if the encoder is push-pull); 1 kΩ series + 1 nF to GND (≈ 160 kHz corner, far above 7 kHz edge rate); TVS/ESD array on the connector; optional Schmitt buffer (*e.g.* 74LVC2G17 class) before the PRU input |
| TB6612 inputs | 33–100 Ω series (edge rate/ringing), 10 kΩ pull-downs, test points |
| IMU SPI | 22–47 Ω series on SCLK/MOSI, short traces, MISO from IMU with 22 Ω; `INT` with 22 Ω to an M4F-capable GPIO; 100 nF + 10 µF at VDD, ferrite from the 3.3 V LDO; IMU module on the stiffest part of the board |
| I2C (INA226, OLED) | 2.2–4.7 kΩ pull-ups to 3.3 V, one set only; keep bus length short |
| E-stop / arm | 3.3 V input with 100 nF debounce + pull-up, ESD diode, polarity-safe 2-pin connector |
| UART | 3.3 V logic only; label TX/RX from the adapter's perspective |
| Status LEDs | power, armed, fault (driven by the M4F), optional buzzer on any PWM-capable pin |

## 9.5 Board and layout guidelines

- **Form:** PB2 plugged into 2×36 female sockets on a larger baseboard (about 90–110 × 60–70 mm),
  not a stacked cape, because the baseboard is wider than the PB2. Do **not** stack the
  GamePup A4 cape; its overlays occupy many of the pins this design needs (doc 1.8). If the
  display is wanted, use the header from row 13.
- **Stack-up:** 4 layers recommended (signal / GND / power / signal), 1.6 mm, 1 oz outer; 2 layers
  is possible but makes the IMU/SPI noise margin worse. Motor and VM traces ≥ 2 mm or pours (3–5 A).
- **Zoning:** power (left), TB6612 and motor connectors (bottom edge), PB2 and digital (centre),
  IMU (centre, away from the buck inductor and motor traces), connectors on the edges.
- **Buck converter:** tight hot loop, inductor shielded, switch node pour minimal, kept ≥ 15 mm from
  the IMU and its SPI traces; ferrite + LC on the 5 V into the PB2.
- **TB6612:** bulk capacitor within 15 mm of VM pins, 100 nF at each VM and VCC pin,
  output pours to the motor connector; thermal vias under the exposed pad if the package has one.
  Copper area is the heat sink: R-11 (doc 6) measures it.
- **Testability:** test points for 5 V, 3.3 V, VM, STBY, PWM, IN1/IN2 (each side), encoder
  A/B (each side), SPI/INT, UART. Two mounting holes with rigid standoffs.
- **IMU placement:** rigid, flat, close to the axle line and body centre, orientation marked
  with the body axes (x forward, y left, z up per doc 4.2). Avoid foam: compliance adds resonances in the loop.
- **Mechanical hints (affects control):** battery mass high on the frame lengthens the pendulum
  (`l ≈ 0.10–0.15 m`) and slows the unstable pole, which makes balancing easier; keep the centre of mass
  over the axle, add front/rear bumpers/skids for falls, and a camera mount with vibration-free fixing.
- **Fabrication files:** KiCad project in `robot/hardware/`; outputs Gerbers, drill, BOM (JLC/LCSC
  columns), pick-and-place; 3D STEP for the chassis designer.

## 9.6 KiCad project and CI for hardware

The Rev 0 project exists in [`robot/hardware/`](../../robot/hardware/README.md): a generated, KiCad 7 module
interconnect schematic (`rev0/balbot_rev0.kicad_sch`), BOM, `pinmap.yaml` (all signals still `tbd`),
`reserved_pins.yaml` (GamePup pins taken from the overlays) and pytest tests.
Workflow `.github/workflows/robot-hardware.yml` runs on changes to `robot/hardware/**`:

- `hardware-tests`: pin-map rules, schematic reproducibility (committed file equals generator output),
  BOM consistency, netlist checks of the STBY gate, driver inputs, encoder paths and rails, and
  a PDF/netlist artifact.
- `erc`: KiCad 8 `kicad-cli sch erc`, **advisory** until it has run clean once (ERC was not available
  in the KiCad 7 used for authoring).
- Rev A adds `kicad-cli pcb drc` (zero errors, no unrouted nets), Gerber/drill/STEP generation, and a check
  that `pinmap.yaml`, the DT overlay and the firmware pin definitions agree.

## 9.7 Design review checklist before ordering Rev A

1. Pin table verified on Rev 0 with a scope (PRU inputs/outputs, MCU SPI, INT, STBY chain).
2. TB6612 VM spike measurement during hard braking at 12.6 V.
3. Buck converter ripple/noise on the 3.3 V rail and IMU noise RMS with motors at 50 % duty.
4. Reverse-polarity and fuse test; BMS trip test.
5. PB2 power-in method confirmed against the PocketBeagle 2 reference manual (5 V entry pin versus USB-C).
6. USB host route to the hub confirmed, with per-port current limits and the camera/dongle stable together.
7. Thermal: TB6612 and buck under 30-minute mixed load.
8. Connectors confirmed against the real motor/battery/camera plugs, with polarity keyed.
