#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Compile both display overlays, apply them in boot order to a stub base tree and
# check that exactly the intended display node is enabled.  Needs dtc + fdtoverlay.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

cat >"$T/base.dts" <<'DTS'
/dts-v1/;
/ { #address-cells=<2>; #size-cells=<2>; compatible="x"; chosen {};
 bus { #address-cells=<1>; #size-cells=<1>;
  main_pmx0: pinctrl@f4000 { compatible="pinctrl-single"; reg=<0xf4000 0x100>; #pinctrl-cells=<1>; pinctrl-single,register-width=<32>; };
  main_gpio0: gpio@600000 { gpio-controller; #gpio-cells=<2>; reg=<0x600000 0x10>; };
  main_gpio1: gpio@601000 { gpio-controller; #gpio-cells=<2>; reg=<0x601000 0x10>; };
  main_spi0: spi@20300000 { #address-cells=<1>; #size-cells=<0>; reg=<0x20300000 0x10>; status="disabled"; };
  main_spi2: spi@20320000 { #address-cells=<1>; #size-cells=<0>; reg=<0x20320000 0x10>; status="disabled"; };
  main_i2c2: i2c@20220000 { #address-cells=<1>; #size-cells=<0>; reg=<0x20220000 0x10>; status="disabled"; };
  ecap2: pwm@23120000 { #pwm-cells=<3>; reg=<0x23120000 0x10>; status="disabled"; };
  epwm2: pwm@23010000 { #pwm-cells=<3>; reg=<0x23010000 0x10>; status="disabled"; };
 }; };
DTS
dtc -@ -q -I dts -O dtb -o "$T/base.dtb" "$T/base.dts"
dtc -@ -q -I dts -O dtb -o "$T/main.dtbo" "$ROOT/k3-am6232-pocketbeagle2-gamepup-a4.dts"
dtc -@ -q -I dts -O dtb -o "$T/fpga.dtbo" "$ROOT/k3-am6232-pocketbeagle2-gamepup-a4-fpga.dts"
dtc -@ -q -I dts -O dtb -o "$T/small.dtbo" "$ROOT/k3-am6232-pocketbeagle2-gamepup-a4-fpga-small.dts"

fdtoverlay -i "$T/base.dtb" -o "$T/lcd.dtb" "$T/main.dtbo"
fdtoverlay -i "$T/base.dtb" -o "$T/fpga.dtb" "$T/main.dtbo" "$T/fpga.dtbo"
fdtoverlay -i "$T/base.dtb" -o "$T/small.dtb" "$T/main.dtbo" "$T/small.dtbo"

get() { fdtget "$@"; }
SPI=/bus/spi@20320000
# lcd: cape display enabled, no fpga node
[ "$(get "$T/lcd.dtb" $SPI/display@0 compatible)" = "adafruit,yx240qv29" ]
! fdtget "$T/lcd.dtb" $SPI/fpga-display@0 compatible >/dev/null 2>&1
! fdtget "$T/lcd.dtb" $SPI/display@0 status >/dev/null 2>&1
# fpga: cape display disabled, panel-mipi-dbi node present with 480x272 timing
[ "$(get "$T/fpga.dtb" $SPI/display@0 status)" = disabled ]
[ "$(get "$T/fpga.dtb" $SPI/fpga-display@0 compatible)" = "gamepup,fpga-lcd480x272 panel-mipi-dbi-spi" ]
[ "$(get "$T/fpga.dtb" $SPI/fpga-display@0/panel-timing hactive)" = 480 ]
[ "$(get "$T/fpga.dtb" $SPI/fpga-display@0/panel-timing vactive)" = 272 ]
fdtget "$T/fpga.dtb" $SPI/fpga-display@0 dc-gpios >/dev/null
fdtget "$T/fpga.dtb" $SPI/fpga-display@0 reset-gpios >/dev/null
# SPI bus itself stays enabled in both
[ "$(get "$T/lcd.dtb" $SPI status)" = okay ]
[ "$(get "$T/fpga.dtb" $SPI status)" = okay ]

# fpga-small: same main display, plus the ST7789 on chip select 3
[ "$(get "$T/small.dtb" $SPI/display@0 status)" = disabled ]
[ "$(get "$T/small.dtb" $SPI/fpga-display@0 compatible)" = "gamepup,fpga-lcd480x272 panel-mipi-dbi-spi" ]
[ "$(get "$T/small.dtb" $SPI/fpga-small-display@3 compatible)" = "gamepup,tn9k-st7789-135x240 panel-mipi-dbi-spi" ]
[ "$(get "$T/small.dtb" $SPI/fpga-small-display@3 reg)" = 3 ]
[ "$(get "$T/small.dtb" $SPI/fpga-small-display@3/panel-timing hactive)" = 240 ]
[ "$(get "$T/small.dtb" $SPI/fpga-small-display@3/panel-timing vactive)" = 135 ]
[ "$(get "$T/small.dtb" $SPI/fpga-small-display@3/panel-timing hback-porch)" = 40 ]
[ "$(get "$T/small.dtb" $SPI/fpga-small-display@3/panel-timing vback-porch)" = 53 ]
[ "$(get "$T/small.dtb" $SPI ti,spi-num-cs)" = 4 ]
# no reset GPIO on the small panel (the FPGA owns its reset); D/C is shared with the main one
! fdtget "$T/small.dtb" $SPI/fpga-small-display@3 reset-gpios >/dev/null 2>&1
[ "$(get "$T/small.dtb" $SPI/fpga-small-display@3 dc-gpios)" = "$(get "$T/small.dtb" $SPI/fpga-display@0 dc-gpios)" ]
# the main display must probe first (it has to stay /dev/fb0): children are probed in node order
order=$(fdtget -l "$T/small.dtb" $SPI | tr '\n' ' ')
case "$order" in "fpga-display@0 fpga-small-display@3 "*) ;; *) echo "bad probe order: $order" >&2; exit 1 ;; esac
# P1.04 pad: CS3 (mode 1) in the small panel's group, left-eye pad no longer owned by the LED group
PMX=/bus/pinctrl@f4000
cs3=$(fdtget -t x "$T/small.dtb" $PMX/gamepup-fpga-small-cs3-pins pinctrl-single,pins)
case " $cs3 " in *" 1a8 20001 "*) ;; *) echo "CS3 pad not set: $cs3" >&2; exit 1 ;; esac
led=$(fdtget -t x "$T/small.dtb" $PMX/gamepup-eye-led-pins pinctrl-single,pins)
case " $led " in *" 16c "* | *" 1a8 "*) echo "LED group still owns P1.04 pads: $led" >&2; exit 1 ;; esac
case " $led " in *" 15c "*) ;; *) echo "right-eye pad missing: $led" >&2; exit 1 ;; esac
# the plain fpga overlay leaves the LED group alone
led0=$(fdtget -t x "$T/fpga.dtb" $PMX/gamepup-eye-led-pins pinctrl-single,pins)
case " $led0 " in *" 16c "*) ;; *) echo "plain fpga overlay changed the LED group" >&2; exit 1 ;; esac
echo "overlay tests passed"
