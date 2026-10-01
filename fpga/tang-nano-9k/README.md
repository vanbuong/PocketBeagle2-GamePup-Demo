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

## Milestone 2 - SPI slave + MIPI-DBI decoder (simulation only)

| file | role |
|---|---|
| `rtl/spi_slave.v` | SPI mode 0, SCLK-domain shifter, D/C sampled at first bit of each byte, toggle hand-off to the system clock |
| `rtl/dbi_decoder.v` | ILI9341 subset (`SWRESET SLPIN/OUT DISPON/OFF CASET PASET RAMWR MADCTL COLMOD`), RGB565 big-endian, window wrap, clips outside 480x272, pointer persists across CS toggles |
| `sim/tb_dbi.v` | SPI master model at ~48 MHz: init sequence, chunked RAMWR, wrap, clip, partial byte, reset |

The decoder outputs a frame-store write port (`wr_en`, `wr_addr = y*480+x`,
`wr_data`). It is not wired into `top.v` yet because the frame store (PSRAM)
does not exist yet; the testbench uses a behavioural array.
Reset (`rst_n`) resets decoder state only; LCD timing keeps running.

## Not yet verified on hardware

- rPLL settings in `pll_pix.v` (VCO 432 MHz, ODIV 48) - confirm in the Gowin IP generator.
- Panel porch/sync values and polarity: check the 4.3" panel datasheet
  (`H_FP/H_SYNC/H_BP`, `V_*`, `*_ACTIVE_LOW` parameters in `lcd_timing.v`).
- LCD pin numbers come from Sipeed's example, not from this board's schematic.
- Expected result: white 1 px border around the full panel, 8 colour bars on
  top, a grey ramp at the bottom, white cross-hair in the centre.

## Next

PSRAM frame store (Gowin IP wrapper) with a write port for the decoder and a line-buffered read port for the LCD scan-out; then pick SPI/DC/reset pins and wire everything into `top.v`.
