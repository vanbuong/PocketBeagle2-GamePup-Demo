# Robot hardware (KiCad)

Source files for the two-wheel balance robot electronics. Design background:
[`docs/balance-robot/09-pcb-design.md`](../../docs/balance-robot/09-pcb-design.md) and
[`08-selected-hardware.md`](../../docs/balance-robot/08-selected-hardware.md).

| Path | What |
|---|---|
| `rev0/balbot_rev0.kicad_pro`, `.kicad_sch` | **Rev 0**: module interconnect schematic for the perfboard build (TB6612 module, MPU-6500 module, INA260, 5 V buck, 3.3 V LDO, 2 × GB37-520 + encoders, E-stop/STBY gate). **Generated** by `tools/gen_rev0.py`; edit the generator, not the `.kicad_sch` |
| `rev0/BOM.csv` | grouped BOM exported from the schematic (`tools/export_bom.py`) |
| `pinmap.yaml` | logical signal -> PocketBeagle 2 header pin. **All 24 signals are `tbd`**: filled during milestone M0 |
| `reserved_pins.yaml` | header pins already used by the GamePup A4 overlays in this repo |
| `tools/` | generator, BOM export, pin-map checker |
| `tests/` | pytest consistency tests (pin map rules, schematic reproducibility, BOM, netlist connectivity) |

Rev A (the real baseboard PCB with PB2 sockets) is **not started**; it waits for the pin map (doc 9.1).

## What Rev 0 contains

- Power: 3S BMS pack -> XT30 -> fuse -> switch -> INA260 shunt path -> +BATT (470 µF + 100 nF + TVS),
  5 V buck module and 3.3 V LDO module as 4-pin headers, PB2 5 V and USB hub power outputs.
- Motor driver: TB6612 as two module headers; 47 Ω series and 10 kΩ pull-down on every driver input.
- **STBY gate**: `STBY = MOT_EN_PRU & MOT_EN_M4F & ESTOP_OK` (74LVC2G08) with a 10 kΩ pull-down; the E-stop
  is a normally-closed loop pulled low by 100 kΩ, so a cut wire stops the motors. Hardware-only path,
  independent of firmware.
- Motors: two 6-pin connectors (harness order must be checked against your motors), 100 nF across each
  motor, encoder RC filter (1 kΩ + 1 nF), optional pull-up (DNP) and 0 Ω link to the signal header.
- Sensors: MPU-6500 SPI module header with 22 Ω series resistors, INA260/INA226 I2C header with pull-ups,
  3.3 V UART header.
- `J18`: 24 logical signals + 2 GND going to the PocketBeagle 2 headers (pins decided in M0).

Not in Rev 0: watchdog IC, USB hub, display headers, PB2 sockets (all Rev A).

## Commands

```sh
pip install -r requirements.txt
sudo apt-get install -y --no-install-recommends kicad kicad-symbols   # KiCad 7 + stock libraries
make            # regenerate schematic + BOM, check pin map, run tests
make pdf        # needs kicad-cli
```

Tests run without KiCad except the netlist test (skipped when `kicad-cli` is missing); the generator needs the
stock symbol libraries (`KICAD_SYMBOL_DIR`, default `/usr/share/kicad/symbols`).

## Status of verification

- Verified here: schematic loads in KiCad 7.0.11, exports a netlist and a PDF, and the netlist tests pass.
- **Not verified:** KiCad ERC (`kicad-cli sch erc` exists only in KiCad 8; CI runs it as an advisory job),
  footprints are placeholders for hand wiring, and no part was checked against a datasheet or on real hardware.
