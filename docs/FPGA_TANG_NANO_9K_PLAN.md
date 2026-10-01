# Plan: Tang Nano 9K as an SPI-slave display (RGB LCD first)

Status: **plan only** - no RTL or overlay changes have been made yet.

## Decisions

| Question | Decision |
|---|---|
| FPGA vs cape LCD | FPGA *replaces* the cape LCD. Selected at boot by which display overlay is enabled (see "Overlay selection"). |
| First output | Tang Nano 9K 40-pin RGB LCD, 4.3" **480x272**. HDMI/DVI comes later. |
| Toolchain | Gowin EDA (free education edition covers GW1NR-9). Keep RTL vendor-neutral; only PLL and PSRAM wrappers are Gowin IP. |

## Approach: the FPGA emulates an ILI9341

The FPGA implements the ILI9341 SPI command set (MIPI-DBI type C, D/C pin
input, RGB565). The kernel's existing tiny ili9341 DRM driver, `/dev/fb0`,
`gamepup-retro`, the menu and benchmarks then work with **no userspace
changes**. Minimum commands: `SWRESET SLPOUT DISPON MADCTL COLMOD CASET PASET
RAMWR`. The DRM driver already sends damaged rectangles only.

## FPGA architecture (GW1NR-9: 27 MHz osc, 468 kb BSRAM, 64 Mb PSRAM)

```
PB2 SPI -> SCLK-domain shift reg -> ILI9341 decoder (D/C, CASET/PASET, addr gen)
        -> async FIFO (BSRAM) -> PSRAM ctrl <-> framebuffer 320x240 RGB565 (153,600 B)
PSRAM -> line buffer (BSRAM) -> RGB timing gen -> 40-pin RGB565 LCD
```

- **PSRAM needed**: one 320x240 RGB565 frame is 1.23 Mb > 468 kb BSRAM.
- **Panel mapping**: 480x272 panel, 320x240 source shown **1:1, centered**
  (80 px side borders, 16 px top/bottom). No scaler needed; a pixel clock of
  ~9 MHz (typ. 4.3" timing: ~525 x 286 total, ~60 Hz - confirm against the
  panel datasheet) leaves PSRAM bandwidth almost unused.
- **SPI slave**: shift register clocked directly by SCLK, CS as frame reset;
  cross to the memory clock with a dual-clock BSRAM FIFO (no oversampling).
- **Tearing**: phase 1 writes the displayed buffer directly (like a real
  panel). Phase 2 adds double buffering with swap at vblank.
- **Reset/D-C**: FPGA takes D/C (P2.17) and reset (P2.19) as inputs.

## SPI budget

32 MHz = 4 MB/s ~ 26 fps full frame; 48 MHz ~ 39 fps. Panel refresh to the LCD
stays 60 Hz independent of SPI. Partial updates (menus) are cheap.

## Overlay selection

Both the cape LCD and the FPGA sit on `main_spi2` CS0 with the same D/C and
reset pins, so they are mutually exclusive:

- `k3-am6232-pocketbeagle2-gamepup-a4.dtbo` - unchanged; cape LCD at 32 MHz.
- new `k3-am6232-pocketbeagle2-gamepup-a4-fpga.dtbo` - loaded *after* the main
  overlay, overrides `display@0` (higher `spi-max-frequency`, no backlight
  dependency) for the FPGA.

`install.sh` / `install-dist.sh` / `cross-build.sh` get a `DISPLAY=lcd|fpga`
option that adds or removes the extra `fdtoverlays` line in extlinux.
Switching = change the option and reboot.

## Milestones

1. `fpga/tang-nano-9k/` skeleton (rtl, constraints, sim, Gowin project/tcl).
   RGB test pattern at 480x272.
2. PSRAM controller (Gowin IP wrapper) + pattern write/readback; framebuffer
   scan-out through line buffer.
3. SPI slave + ILI9341 decoder, simulated (Verilator/cocotb) against a captured
   DRM init + frame stream. `tools/send_frame.py` (spidev) for bench tests.
4. Overlay variant + install option; run menu, NES, Doom on the LCD.
5. Phase 2: 40-48 MHz SPI, dirty-rect commits, double buffering, optional
   HDMI/DVI output.
6. Docs: wiring table (P1.06/08/12, P2.17/19, GND), README section.

## Risks / open items

- Gowin EDA is hard to run in GitHub CI (download/account); plan is to build
  the bitstream locally and commit a release `.fs`, keeping RTL sim in CI.
- 48 MHz SPI over jumper wires may need short leads or series resistors;
  fall back to 32 MHz.
- Confirm Tang Nano 9K RGB connector pinout and the 4.3" panel timing/polarity
  from the Sipeed schematic and panel datasheet before writing constraints.
- Cape LCD must be disconnected (shared SPI/D-C/reset lines) when the FPGA is
  wired in.
