# Hardware bring-up ladder (Tang Nano 9K display)

Nothing here had been run on hardware when this was written. Test one step at a time; each step
lists the commit to check out, what to wire, and what you should see.

**Rule of thumb:** every commit contains all earlier work, so check out the *software* commit of
the step you are testing, and always build the *gateware* (`fpga/tang-nano-9k`) from
**`7060903` or newer**. The gateware in older commits has bugs found later (see the table), so
testing those commits on hardware only re-finds known problems.

## All commits since the original repo

| commit | change | hardware-testable? |
|---|---|---|
| `876a23f` | original repo (cape LCD, menu, OLED) | yes: **baseline** (step 0) |
| `20140d2`, `b873554` | plan documents | no |
| `1ab92f0` | FPGA skeleton, 480x272 test pattern | gateware has the **PLL bug** (wrong dividers, "Invalid VCO"): pixel clock wrong. Usable only with the 2-line fix (`IDIV_SEL(2)`, `FBDIV_SEL(0)`, `ODIV_SEL(48)` in `pll_pix.v`) |
| `6d30e9e` | SPI slave + MIPI-DBI decoder | simulation only (not wired into the top); PLL bug |
| `4b5fed0` | frame store, BSRAM, full SPI->LCD path | PLL bug; BSRAM store **does not fit the chip**; async-reset flip-flop Gowin cannot build |
| `860bfd9` | Linux: FPGA overlay, panel firmware, `gamepup-display`, installer | **yes, Linux side** (step 3) |
| `769161b` | PSRAM frame store | same bugs as `4b5fed0` |
| `16bdf59` | BSRAM store fits, async-reset fixed | PLL bug only |
| `d2c9507` | block-RAM attributes | PLL bug only |
| `3fdd7fd` | userspace follows 320x240 / 480x272 | **yes** (step 4) |
| `0b20f7e` | on-board ST7789 (FPGA bridge + overlay + firmware) | **yes, Linux side** (step 6); its gateware has the PLL bug |
| `248ae62` | status display on the ST7789 | **yes** (step 7) |
| `72d6782` | open-source CI, **PLL fixed** | first gateware you can trust the clock of (still SDC/width warnings in Gowin) |
| `7060903` | SDC error and width warnings fixed, Gowin IDE how-to | **use this (or newer) for all gateware** |

## Step 0 - baseline, cape LCD only (`876a23f`)

Check the original system works before touching it: install as in the README, boot, the menu runs
on the cape ILI9341, buttons work. Later steps can then be compared against this.

## Step 1 - FPGA drives the RGB LCD with a test pattern (gateware `7060903`)

No PocketBeagle 2 involved. Gowin EDA project for GW1NR-LV9QN88PC6/I5, files and top module as in
`fpga/tang-nano-9k/README.md` ("Building in the Gowin EDA IDE"): top module **`top_bsram`**, and in
`rtl/top_bsram.v` change `parameter SHOW_PATTERN = 0` to **`1`**. Plug the 4.3" 480x272 panel into the
40-pin connector, program the board.

Expect: a 1-pixel white border around the whole panel, 8 colour bars over the top two thirds,
a grey ramp below, a white cross-hair in the centre.
If it fails: no image or rolling picture -> panel porches/polarity (`H_FP`, `H_SYNC`, `H_BP`,
`V_*`, `*_ACTIVE_LOW` in `lcd_timing.v`, check the panel datasheet); wrong colours -> pin order in the
`.cst`; picture shifted -> porch values; unstable -> PLL (look at the Gowin timing/clock report).

## Step 2/3 - PocketBeagle 2 talks to the FPGA (Linux `860bfd9`, gateware `7060903`)

Gateware: same project, `top_bsram`, `SHOW_PATTERN = 0` (8 colours). Disconnect the cape LCD (it shares
the SPI/D-C/reset lines). Wire (3.3 V, common ground):

| signal | PB2 | Tang Nano 9K |
|---|---|---|
| SCLK | P1.08 | IO36 (shares the microSD clock; leave the slot empty) |
| MOSI | P1.12 | IO25 |
| CS0 | P1.06 | IO27 |
| D/C | P2.17 | IO28 |
| reset | P2.19 | IO29 |
| GND | GND | GND |

On the PB2: `git checkout 860bfd9`, `sudo ./install.sh`, `sudo gamepup-display fpga`, reboot.

Check: `dmesg | grep -i -E "mipi|panel|firmware"` (a "No config file found" means
`/lib/firmware/gamepup,fpga-lcd480x272.bin` is missing; a probe error means the `panel-mipi-dbi`
module or wiring), `cat /sys/class/graphics/fb*/virtual_size` (expect `480,272`),
`cat /dev/urandom > /dev/fb0` (expect coloured noise in 8 colours), and the Linux console text.
If the panel stays black: lower `spi-max-frequency` in the overlay (try 8-16 MHz), check D/C and CS wiring.

Also check the way back: `sudo gamepup-display lcd`, reconnect the cape LCD, reboot: the original
display must work again.

At this commit the menu and games still expect 320x240, so they will refuse the 480x272 screen.

## Step 4 - userspace at 480x272 (`3fdd7fd`)

First on the **cape LCD** (`gamepup-display lcd`): menu, hardware test, a game, GPU benchmark must look
exactly as in step 0. Then on the FPGA display: the menu fills 480x272 (19 rows), hardware test centred,
games letterboxed (NES 290x272, Doom 4:3).
With the 8-colour BSRAM build the colours are crude (each channel is 1 bit): the menu's blue selection bar
and dark green rule come out **black** and grey becomes green, so judge layout, not colours. Frame rate is
limited by SPI (about 15 fps for a full frame at 32 MHz).

## Step 5 - full colour from the PSRAM (gateware `7060903` + Gowin PSRAM IP)

Generate the IP (`fpga/tang-nano-9k/ip/psram/README.md`), build `top_psram`, same wiring and Linux setup
as step 3. Check in this order, it tells you where a problem is: the panel is **black** after power-up
(frame cleared) -> console text appears (writes work) -> colours/menu correct (read path). Banding or
shifted pixels point at word/byte order or `ADDR_SHIFT`; a frozen picture at the IP's command timing. The
README section "What the simulation does NOT prove" lists every assumption made about the Gowin IP.

## Step 6 - on-board 1.14" ST7789 (Linux `0b20f7e`, gateware `7060903`)

Extra wire: PB2 **P1.04 -> Tang Nano IO26** (chip select 3). `sudo gamepup-display fpga-small`, reboot.
Expect two framebuffers (`480,272` and `240,135`) and `cat /dev/urandom > /dev/fbN` on the 240x135 one
fills the small panel. The cape's left-eye LED stops working in this mode. Shifted or mirrored picture ->
`vback-porch`/`hback-porch` in the overlay and MADCTL in `fpga/linux/gamepup,tn9k-st7789-135x240.txt`.

## Step 7 - status dashboard on the ST7789 (`248ae62`)

`sudo ./install.sh`, then `systemctl restart gamepup-oled-status`; `journalctl -u gamepup-oled-status`
should say it uses the 240x135 framebuffer. Expect the colour dashboard (bars turn yellow/red with load),
the encoder changes brightness, a click shows the GIF in colour.
