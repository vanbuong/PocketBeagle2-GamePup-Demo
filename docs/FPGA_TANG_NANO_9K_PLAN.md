# Plan: Tang Nano 9K as an SPI-slave display (RGB LCD first)

Status: **plan only** - no RTL or overlay changes have been made yet.

## Decisions

| Question | Decision |
|---|---|
| FPGA vs cape LCD | FPGA *replaces* the cape LCD. Selected at boot by which display overlay is enabled (see "Overlay selection"). |
| First output | Tang Nano 9K 40-pin RGB LCD, 4.3" **480x272**. HDMI/DVI comes later. |
| Toolchain | Gowin EDA (free education edition covers GW1NR-9). Keep RTL vendor-neutral; only PLL and PSRAM wrappers are Gowin IP. |

## Decision: native 480x272 framebuffer end to end

The Linux framebuffer is **480x272 RGB565** (not 320x240 centered). The
panel, the FPGA frame store and `/dev/fb0` all use the same size, so nothing
is scaled or bordered in hardware.

## Approach: FPGA as a generic MIPI-DBI (type C) panel

The tiny `ili9341` DRM driver is fixed at 240x320, so it cannot expose
480x272. Instead use the kernel's generic **`panel-mipi-dbi`** DRM driver
(`compatible = "panel-mipi-dbi-spi"`): resolution, timings and the init
command sequence come from a small firmware blob (`/lib/firmware/*.bin`,
generated with the kernel's `mipi-dbi-cmd` script from a text file with
`width-mm`, `hactive=480`, `vactive=272`, etc). The FPGA implements the
MIPI-DBI command subset (D/C pin input, RGB565):
`SWRESET SLPOUT DISPON MADCTL COLMOD CASET PASET RAMWR`, with CASET/PASET
accepting 16-bit coordinates up to 479/271. `/dev/fb0` and the DRM
damage-rect updates work as today.

Repo build impact: `drm_mipi_dbi.o` is already built out of tree; add
`panel-mipi-dbi.c` to `Makefile`/`scripts/cross-build.sh` (or rely on the
in-kernel module if the Armbian kernel ships it - check `CONFIG_DRM_PANEL_MIPI_DBI`).

## FPGA architecture (GW1NR-9: 27 MHz osc, 468 kb BSRAM, 64 Mb PSRAM)

```
PB2 SPI -> SCLK-domain shift reg -> ILI9341 decoder (D/C, CASET/PASET, addr gen)
        -> async FIFO (BSRAM) -> PSRAM ctrl <-> framebuffer 320x240 RGB565 (153,600 B)
PSRAM -> line buffer (BSRAM) -> RGB timing gen -> 40-pin RGB565 LCD (480x272)
```

- **PSRAM needed**: one 480x272 RGB565 frame is 261,120 B = 2.09 Mb >
  468 kb BSRAM. Two buffers (4.2 Mb) fit easily in the 64 Mb PSRAM.
- **Panel mapping**: frame is 1:1 with the panel, no scaler. Pixel clock
  ~9 MHz (typ. 4.3" timing ~525 x 286 total, ~60 Hz - confirm against the
  panel datasheet) leaves PSRAM bandwidth almost unused.
- **SPI slave**: shift register clocked directly by SCLK, CS as frame reset;
  cross to the memory clock with a dual-clock BSRAM FIFO (no oversampling).
- **Tearing**: phase 1 writes the displayed buffer directly (like a real
  panel). Phase 2 adds double buffering with swap at vblank.
- **Reset/D-C**: FPGA takes D/C (P2.17) and reset (P2.19) as inputs.

## SPI budget

Full frame = 261,120 B. 32 MHz (4 MB/s) ~ 15 fps; 48 MHz (6 MB/s) ~ 23 fps.
That is a drop from the 320x240 panel (26 fps at 32 MHz), so full-screen
games are limited by SPI, not by the FPGA. Panel refresh stays 60 Hz.
Mitigations, in order: (1) 48 MHz SPI; (2) dirty-rect updates (menus, UI);
(3) phase 2 custom protocol with RGB332/RGB444 (1.3-1.6x fewer bytes);
(4) games keep a 320x240 or smaller render area inside the 480x272 frame so
damage is limited to the game rect, or the FPGA does 1.5x/2x upscaling of a
sub-window (source 240x136 or 320x180 etc.) so the SPI carries fewer pixels.

## Overlay selection

Both the cape LCD and the FPGA sit on `main_spi2` CS0 with the same D/C and
reset pins, so they are mutually exclusive:

- `k3-am6232-pocketbeagle2-gamepup-a4.dtbo` - unchanged; cape LCD at 32 MHz.
- new `k3-am6232-pocketbeagle2-gamepup-a4-fpga.dtbo` - loaded *after* the main
  overlay, overrides `display@0`: `compatible = "panel-mipi-dbi-spi"`,
  `spi-max-frequency` 40-48 MHz, `width-mm`/`height-mm`, no `rotation`, no
  backlight dependency. The firmware blob is installed to `/lib/firmware`.

**Implemented:** the FPGA overlay node is `fpga-display@0` (the cape's `display@0`
is set `status = "disabled"`, so no property deletion is needed). `gamepup-display
lcd|fpga|status` (`scripts/gamepup-display`) edits the single `fdtoverlays` line;
`install.sh` / `install-dist.sh` accept `GAMEPUP_DISPLAY=lcd|fpga`;
`cross-build.sh` ships the overlay, `/lib/firmware/gamepup,fpga-lcd480x272.bin`
(from `fpga/linux/`) and the selector. `panel-mipi-dbi` is built out of tree in a
separate, non-fatal pass. `scripts/test-overlays.sh` (run in CI) merges both
overlays onto a stub tree and checks which display node is enabled.
Switching = run `gamepup-display` and reboot.

## Milestones

1. `fpga/tang-nano-9k/` skeleton (rtl, constraints, sim, Gowin project/tcl).
   RGB test pattern at 480x272.
2. PSRAM controller (Gowin IP wrapper) + pattern write/readback; framebuffer
   scan-out through line buffer.
3. SPI slave + ILI9341 decoder, simulated (Verilator/cocotb) against a captured
   DRM init + frame stream. `tools/send_frame.py` (spidev) for bench tests.
4. Overlay variant + firmware blob + `panel-mipi-dbi` module + install option.
5. Userspace port to 480x272: `emulator/gamepup-menu`, `gamepup-hardware-test`,
   `gamepup-retro.c` and `gamepup-gpu-bench.c` hardcode 320x240 (`fb_width`,
   `fb_height`, pbuffer sizes); make them read the size from the framebuffer,
   regenerate the `bezels/*.rgb` at 480x272, update README render-resolution
   table. Then run menu, NES, Doom on the LCD.
6. Phase 2: 40-48 MHz SPI, dirty-rect commits, double buffering, optional
   HDMI/DVI output.
7. Docs: wiring table (P1.06/08/12, P2.17/19, GND), README section.

## Status

Implemented in `fpga/tang-nano-9k/` (simulation only, nothing run on hardware yet):
test pattern, SPI slave + MIPI-DBI decoder, BSRAM 8-colour frame store, PSRAM
full-colour frame store (needs the Gowin PSRAM IP, generated locally), FPGA overlay
+ firmware + `gamepup-display`. Remaining: board bring-up, then the userspace
480x272 port.

## Risks / open items

- Gowin EDA is hard to run in GitHub CI (download/account); plan is to build
  the bitstream locally and commit a release `.fs`, keeping RTL sim in CI.
- 48 MHz SPI over jumper wires may need short leads or series resistors;
  fall back to 32 MHz.
- Confirm Tang Nano 9K RGB connector pinout and the 4.3" panel timing/polarity
  from the Sipeed schematic and panel datasheet before writing constraints.
- `panel-mipi-dbi` needs a kernel with that driver (6.2+); it must be built
  out of tree like `ili9341` if the Armbian kernel lacks it.
- Cape LCD must be disconnected (shared SPI/D-C/reset lines) when the FPGA is
  wired in.
