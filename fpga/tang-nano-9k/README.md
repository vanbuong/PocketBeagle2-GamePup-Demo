# Tang Nano 9K display slave

FPGA side of the GamePup display path (see `docs/FPGA_TANG_NANO_9K_PLAN.md`).

## Milestone 1 - 480x272 RGB LCD test pattern (this directory)

`rtl/top.v` drives the 40-pin RGB LCD connector (4.3" 480x272, RGB565) from
the 27 MHz crystal through a 9 MHz pixel clock:

| file | role |
|---|---|
| `rtl/pll_pix.v` | 27 -> 9 MHz (Gowin rPLL, behavioural model under `-DSIM`) |
| `rtl/lcd_timing.v` | 525x286 timing, DE + HS/VS, x/y counters |
| `rtl/test_pattern.v` | border, 8 colour bars, grey ramp, cross-hair |
| `rtl/top.v` | glue + pins |
| `constraints/` | pin `.cst` and timing `.sdc` |
| `sim/tb_lcd_timing.v` | checks 525x286 frame, 480x272 active, border pixels |

```sh
make sim        # needs iverilog; prints PASS
make bitstream  # needs Gowin EDA (gw_sh) in PATH
make program    # needs openFPGALoader
```

Or open Gowin EDA, create a GW1NR-LV9QN88PC6/I5 project and add the files.

## Not yet verified on hardware

- rPLL settings in `pll_pix.v` (VCO 432 MHz, ODIV 48) - confirm in the Gowin IP generator.
- Panel porch/sync values and polarity: check the 4.3" panel datasheet
  (`H_FP/H_SYNC/H_BP`, `V_*`, `*_ACTIVE_LOW` parameters in `lcd_timing.v`).
- LCD pin numbers come from Sipeed's example, not from this board's schematic.
- Expected result: white 1 px border around the full panel, 8 colour bars on
  top, a grey ramp at the bottom, white cross-hair in the centre.

## Next

SPI slave + MIPI-DBI decoder, PSRAM frame store, scan-out from PSRAM.
