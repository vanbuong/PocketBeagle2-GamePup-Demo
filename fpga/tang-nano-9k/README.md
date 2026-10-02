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

## Milestone 3 - frame store interface + BSRAM bring-up version

`top.v` is now the full path (`SHOW_PATTERN=0`, default; set 1 for the
milestone-1 test pattern):

```
SPI_SCK/MOSI/CS_N + DBI_DC --> spi_slave --> dbi_decoder --> framestore --> lcd_scanout --> LCD pins
DBI_RST_N -----------------------------^   (decoder state only)
```

Frame store port list (the PSRAM version will keep it):

| port | clock | meaning |
|---|---|---|
| `wr_en, wr_addr[16:0], wr_data[15:0]` | `clk_w` (27 MHz) | RGB565 pixel at `y*480+x` |
| `rd_addr[16:0]` -> `rd_data[15:0]` | `clk_r` (9 MHz pixel) | RGB565, 1 clock latency |

`rtl/framestore_bsram.v` is the bring-up version. A full 480x272 RGB565 frame
(2.09 Mb) does not fit the 468 kb of BSRAM, so it keeps only the top
`R_BITS/G_BITS/B_BITS` of each channel (default 1/1/1 = **8 colours**,
391,680 bits ~ 24 of 26 BSRAM blocks) and re-expands to RGB565 on read.
Good enough to see the Linux console/menu and verify SPI, DC, reset, windows
and timing on real hardware before the PSRAM controller exists.
`display_on` (DISPON/DISPOFF) blanks the LCD output.

SPI pins (`constraints/tangnano9k_lcd.cst`), 3.3 V, taken from a working
Tang Nano 9K project, not from the schematic:

| signal | Tang Nano 9K pin | PocketBeagle 2 (cape wiring) |
|---|---|---|
| SPI_SCK | IO36 (shares microSD clock - keep SD slot empty) | P1.08 |
| SPI_MOSI | IO25 | P1.12 |
| SPI_CS_N | IO27 | P1.06 |
| SPI_CS3_N (small display, optional) | IO26 | P1.04 |
| DBI_DC | IO28 | P2.17 |
| DBI_RST_N | IO29 | P2.19 |
| GND | GND | GND |

`sim/tb_top.v` drives SPI into `top`, captures a frame from the LCD pins and
checks pixels, blanking after DISPOFF. Mutation-tested (address off-by-one and
ignoring display_on both fail it).

## Milestone 4 - PSRAM frame store, full RGB565

`framestore_psram` has the same write port as the BSRAM store; the read port is now
`rd_x`/`rd_y` (the `lcd_timing` counters) for both stores. It stores the full 480x272
frame (261,120 bytes) in the on-chip 64 Mbit PSRAM through Gowin's **PSRAM Memory
Interface HS** IP (burst length 16 = 4 x 64-bit beats = 16 pixels).

```
decoder (27 MHz) -> async_fifo -> psram_ctrl (clk_out 74.25 MHz) <-> Gowin PSRAM IP <-> PSRAM
                                      | line fetch                   
                          line buffer (2 x 480 px, BSRAM) -> LCD scan-out (9 MHz)
```

| file | role |
|---|---|
| `rtl/async_fifo.v` | gray-coded dual-clock FIFO for pixel writes (64 deep) |
| `rtl/psram_ctrl.v` | write combiner (16-px bursts with byte masks), line prefetch, startup clear |
| `rtl/framestore_psram.v` | CDCs, line buffer, IP instance |
| `rtl/pll_mem.v` | 27 -> 148.5 MHz memory clock |
| `rtl/top_psram.v`, `rtl/top_bsram.v` | synthesis tops (PSRAM pads only on the PSRAM one) |
| `sim/psram_ip_model.v` | behavioural stand-in for the Gowin IP (simulation only) |
| `ip/psram/` | put the generated Gowin IP here (not in the repo) |

How it works: SPI pixels are merged into 16-pixel bursts (byte-masked, so partial
bursts leave neighbours alone; flushed on burst change, full burst or 24 idle
clocks). At the start of every display line the pixel domain asks for the *next*
line, which `psram_ctrl` reads (30 bursts) into the ping-pong line buffer. Reads and
writes alternate when both are pending. After calibration the frame is cleared to
black (~1.5 ms).

### PSRAM build

1. Generate the IP as described in `ip/psram/README.md` (burst length 16).
2. `make bitstream-psram` (`gw_sh build.tcl psram`), then flash
   `impl/pnr/tangnano9k_lcd_psram.fs`.
3. Switch the SPI/DBI wiring and overlay exactly as for the BSRAM build.

`make sim` runs the PSRAM path against `sim/psram_ip_model.v` (full-colour exact
compare, a partial-burst/byte-mask case, command-spacing and alignment checks).
Mutation-tested: wrong command spacing, wrong pixel select and ignored byte masks
each fail it.

### What the simulation does NOT prove (check on hardware)

The Gowin IP is encrypted and was not available here; the model is written from
Gowin's example for this IP, so a wrong assumption would be in both. Verify against
IPUG943 / on the board:

- `cmd` polarity (1 = write), data beat 0 in the same cycle as `cmd_en`, beats 1..3
  in the next three cycles, `data_mask` 1 = masked, byte order little-endian.
- `addr` unit: assumed one 16-bit word (pixel index). If it is a byte address, set
  `ADDR_SHIFT = 1` on `psram_ctrl` (via `framestore_psram`).
- Minimum command spacing `T_CMD = 14` clocks for burst 16 (Gowin's example) and
  read latency; `psram_ctrl` waits for all 4 read beats and sets `err_timeout` if the
  IP never answers.
- `clk_out` is 74.25 MHz with a 148.5 MHz `memory_clk`; `pll_mem.v` parameters pass the
  VCO/PFD range check in CI but the output frequency is not tested on hardware.
  No timing constraints exist yet for `clk_out` in `constraints/tangnano9k.sdc`.
- Gowin must map the 64-bit line buffer (256 x 64) to BSRAM and the FIFO to LUT RAM.

If the picture is wrong: first check that the screen is black after reset (clear
works), then that rows from `fbi`/the console appear (writes), then banding or
shifted pixels (word/byte order or `ADDR_SHIFT`).

## CI and the open-source flow

`.github/workflows/fpga.yml` runs on every change under `fpga/`:

| job | what it checks |
|---|---|
| `simulate` | `make sim`: all testbenches (iverilog) |
| `bitstream` | `ci/build-oss.sh all` with yosys + nextpnr-himbaechel + apycula (pinned in `ci/requirements.txt`, installed from PyPI): <br>- both PLLs' parameters are accepted by `gowin_pack` (VCO/PFD ranges)<br>- the **BSRAM build is placed & routed, must meet timing at 50 MHz, and packed into `tangnano9k_lcd.fs`**, uploaded as an artifact<br>- the **PSRAM build is synthesised** with the Gowin IP black-boxed and must fit the chip |
| `gowin-eda` | optional, manual (`run_gowin`), **self-hosted runner labelled `gowin`** with Gowin EDA installed: `gw_sh build.tcl` (+ `psram` if the IP is in `ip/psram/`) |

Gowin EDA is not redistributable, so GitHub-hosted runners cannot run it; the open-source flow
builds the real BSRAM bitstream instead. The full-colour PSRAM bitstream needs Gowin's encrypted IP,
so it is only built by the self-hosted job. Locally: `pip install -r ci/requirements.txt && make bitstream-oss`
(a few minutes, output in `build/oss/`).

Last results (open-source flow): BSRAM build 474 of 8,640 LUT4, 244 flip-flops, 24 of 26 block RAMs,
timing met with large margin (system clock Fmax about 157 MHz, SPI clock about 235 MHz); PSRAM build
(synthesis only) about 1,330 LUTs, 1,060 flip-flops, 2 block RAMs. An earlier estimate from an old
yosys (~6,000 LUTs) was too pessimistic. The open-source flow is not Gowin EDA: its numbers are
estimates for Gowin EDA, and not a hardware test. nextpnr ignores the `.sdc`; every clock is checked
against one `FREQ` (default 50 MHz).

Bugs this found: a single wide memory mapped to ~94,000 cells (now 24 block RAMs); a flip-flop with a
non-constant async reset that Gowin flops cannot implement; and **wrong PLL divider settings** (the
VCO came out at 20 GHz): fixed to `IDIV=2/FBDIV=0/ODIV=48` for 9 MHz and `IDIV=1/FBDIV=10/ODIV=4` for 148.5 MHz.

## Milestone 5 - on-board 1.14" ST7789 as a second display

The Tang Nano 9K's small 240x135 ST7789 SPI LCD can be driven by the PocketBeagle 2
through the same wires as the main display, with its own chip select (**PB2 P1.04 =
SPI2_CS3**). No SPI master is needed: `rtl/small_lcd_bridge.v` passes SCLK, MOSI and D/C
straight through to the panel and gates it with CS3 (the ST7789 ignores SCL/SDA while CS
is high, so main-display traffic on the shared lines does nothing). Linux sees a second
ordinary SPI panel (`panel-mipi-dbi`, firmware `gamepup,tn9k-st7789-135x240.bin`).
The main-display SPI slave and decoder are not involved.

Extra wiring (the panel itself is on the board; pins from its schematic):

| signal | Tang Nano 9K | PocketBeagle 2 |
|---|---|---|
| CS3 (small display select) | IO26 (header) | P1.04 |
| SCLK / MOSI / D/C | shared with the main display (IO36 / IO25 / IO28) | P1.08 / P1.12 / P2.17 |
| ST7789 SCL, SDA, RS, CS | pins 76, 77, 49, 48 (on board) | - |
| ST7789 reset | pin 47, driven open-drain by the FPGA (low for 100 ms after configuration, then released) | not connected |

`sim/tb_small_lcd.v` checks the pass-through of every line and the reset timing.

Things to know:

- **P1.04 is also the GamePup cape's left-eye LED pin.** `gamepup-display fpga-small`
  takes that pad for CS3, so the left eye stops working (and may blink with small-display
  traffic). The right eye is unchanged.
- The ST7789 has no reset line from the PB2: the panel node has no `reset-gpios` and the
  init sequence starts with a software reset. The FPGA resets the panel once at power-up,
  so configure the FPGA before (or while) Linux boots.
- The init sequence, the 240x135 window offsets (column 40, row 53, MADCTL `0x70`) and
  inversion come from Sipeed's `spi_lcd` example for this exact panel. If the picture is
  shifted or mirrored, adjust `vback-porch`/`hback-porch` in the overlay and MADCTL in
  `fpga/linux/gamepup,tn9k-st7789-135x240.txt`.
- The SPI clock for the small panel starts at 32 MHz like the main display.

## Not yet verified on hardware

- rPLL settings in `pll_pix.v` (9 MHz): valid per apycula, not checked on hardware.
- Panel porch/sync values and polarity: check the 4.3" panel datasheet
  (`H_FP/H_SYNC/H_BP`, `V_*`, `*_ACTIVE_LOW` parameters in `lcd_timing.v`).
- LCD pin numbers come from Sipeed's example, not from this board's schematic.
- Gowin must infer the 3 x 1-bit x 130,560 memory as BSRAM (check the resource report; fall back to explicit SDPB primitives if it uses LUT/FF), and accept the `expand` function.
- Expected result (pattern mode): white 1 px border around the full panel, 8 colour bars on
  top, a grey ramp at the bottom, white cross-hair in the centre.

## Next

bring-up on the board (BSRAM, then PSRAM), then the userspace 480x272 port.
