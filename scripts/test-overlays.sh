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

fdtoverlay -i "$T/base.dtb" -o "$T/lcd.dtb" "$T/main.dtbo"
fdtoverlay -i "$T/base.dtb" -o "$T/fpga.dtb" "$T/main.dtbo" "$T/fpga.dtbo"

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
echo "overlay tests passed"
