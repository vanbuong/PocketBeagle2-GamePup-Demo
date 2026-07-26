#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "Run this installer as root." >&2
	exit 1
fi

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
KERNEL_VERSION=$(uname -r)
UPSTREAM_VERSION=v${KERNEL_VERSION%%-*}
KERNEL_CC=${KERNEL_CC:-gcc-14}
MODULE_SOURCE_DIR=/usr/src/gamepup-st7735r-$KERNEL_VERSION
MODULE_INSTALL_DIR=/lib/modules/$KERNEL_VERSION/updates/gamepup
OVERLAY_NAME=k3-am6232-pocketbeagle2-gamepup-a4
OVERLAY_TARGET=/boot/dtb/ti/$OVERLAY_NAME.dtbo
EXTLINUX_CONFIG=/boot/extlinux/extlinux.conf
DEVICE_USER=${DEVICE_USER:-beagle}

missing_packages=
for package in build-essential ca-certificates curl device-tree-compiler \
	dosfstools gcc-14 libegl-dev libgif-dev libgles-dev; do
	if ! dpkg-query -W -f='${db:Status-Abbrev}' "$package" 2>/dev/null | \
		grep -q '^ii'; then
		missing_packages="$missing_packages $package"
	fi
done
if [ -n "$missing_packages" ]; then
	apt-get update
	# shellcheck disable=SC2086
	apt-get install -y --no-install-recommends $missing_packages
fi

if [ ! -e "/lib/modules/$KERNEL_VERSION/build/Makefile" ]; then
	echo "Kernel headers for $KERNEL_VERSION are missing." >&2
	echo "Install the matching Armbian kernel-header package, then retry." >&2
	exit 1
fi

if ! command -v "$KERNEL_CC" >/dev/null 2>&1; then
	echo "$KERNEL_CC is required because this Armbian kernel was built with GCC 14." >&2
	exit 1
fi

if [ ! -e /usr/local/include/libretro.h ]; then
	echo "The libretro API header is missing." >&2
	echo "Run emulator/install-cores.sh before install.sh." >&2
	exit 1
fi

install -d -m 0755 "$MODULE_SOURCE_DIR" "$MODULE_INSTALL_DIR"
install -m 0644 "$SCRIPT_DIR/Makefile" "$MODULE_SOURCE_DIR/Makefile"

curl -fsSLo "$MODULE_SOURCE_DIR/drm_mipi_dbi.c" \
	"https://raw.githubusercontent.com/gregkh/linux/$UPSTREAM_VERSION/drivers/gpu/drm/drm_mipi_dbi.c"
curl -fsSLo "$MODULE_SOURCE_DIR/st7735r.c" \
	"https://raw.githubusercontent.com/gregkh/linux/$UPSTREAM_VERSION/drivers/gpu/drm/tiny/st7735r.c"

make -C "/lib/modules/$KERNEL_VERSION/build" M="$MODULE_SOURCE_DIR" \
	CC="$KERNEL_CC" modules
install -m 0644 "$MODULE_SOURCE_DIR/drm_mipi_dbi.ko" "$MODULE_INSTALL_DIR/"
install -m 0644 "$MODULE_SOURCE_DIR/st7735r.ko" "$MODULE_INSTALL_DIR/"
depmod -a "$KERNEL_VERSION"

dtc -@ -I dts -O dtb -o "$OVERLAY_TARGET" \
	"$SCRIPT_DIR/$OVERLAY_NAME.dts"

if ! getent group spi >/dev/null 2>&1; then
	groupadd --system spi
fi
install -m 0644 "$SCRIPT_DIR/99-gamepup-peripherals.rules" \
	/etc/udev/rules.d/99-gamepup-peripherals.rules
if id "$DEVICE_USER" >/dev/null 2>&1; then
	usermod -a -G spi,i2c "$DEVICE_USER"
fi
udevadm control --reload-rules
udevadm trigger --subsystem-match=spidev

install -d -m 0755 /usr/local/bin /usr/local/libexec /usr/local/sbin
make -C "$SCRIPT_DIR/emulator" all
install -m 0755 "$SCRIPT_DIR/emulator/gamepup-retro" \
	/usr/local/bin/gamepup-retro
install -m 0755 "$SCRIPT_DIR/emulator/gamepup-oled-status" \
	/usr/local/bin/gamepup-oled-status
install -m 0755 "$SCRIPT_DIR/emulator/gamepup-gpu-bench" \
	/usr/local/bin/gamepup-gpu-bench
install -d -m 0755 /opt/gamepup/gifs
install -m 0644 "$SCRIPT_DIR/emulator/gifs/"*.gif /opt/gamepup/gifs/
install -d -m 0755 /usr/local/share/gamepup/bezels
install -m 0644 "$SCRIPT_DIR/emulator/bezels/"*-system.rgb \
	/usr/local/share/gamepup/bezels/
install -m 0644 "$SCRIPT_DIR/emulator/bezels/GENERATION.md" \
	/usr/local/share/gamepup/bezels/
install -m 0644 "$SCRIPT_DIR/emulator/gamepup-oled-status.service" \
	/etc/systemd/system/gamepup-oled-status.service
install -m 0755 "$SCRIPT_DIR/emulator/gamepup-menu" \
	/usr/local/bin/gamepup-menu
install -m 0755 "$SCRIPT_DIR/emulator/gamepup-hardware-test" \
	/usr/local/bin/gamepup-hardware-test
install -m 0755 "$SCRIPT_DIR/emulator/gamepup-rom-import" \
	/usr/local/libexec/gamepup-rom-import
install -m 0755 "$SCRIPT_DIR/emulator/gamepup-rom-drive-setup" \
	/usr/local/libexec/gamepup-rom-drive-setup
install -m 0755 "$SCRIPT_DIR/emulator/gamepup-rom-import-watch" \
	/usr/local/libexec/gamepup-rom-import-watch
install -m 0755 "$SCRIPT_DIR/emulator/gamepup-backlight-permissions" \
	/usr/local/libexec/gamepup-backlight-permissions
install -m 0755 "$SCRIPT_DIR/emulator/gamepup-fbcon" \
	/usr/local/libexec/gamepup-fbcon
install -m 0755 "$SCRIPT_DIR/emulator/pb2-usb-gadget-ncm-acm-storage" \
	/usr/local/sbin/pb2-usb-gadget-ncm-acm
install -d -m 0755 /usr/local/share/gamepup/rom-drive /opt/gamepup/saves
install -m 0644 "$SCRIPT_DIR/emulator/rom-drive/README.txt" \
	/usr/local/share/gamepup/rom-drive/README.txt
install -m 0440 "$SCRIPT_DIR/emulator/gamepup-rom-import.sudoers" \
	/etc/sudoers.d/gamepup-rom-import
install -m 0644 "$SCRIPT_DIR/emulator/gamepup-rom-import-watch.service" \
	/etc/systemd/system/gamepup-rom-import-watch.service
install -m 0644 "$SCRIPT_DIR/emulator/gamepup-game.service" \
	/etc/systemd/system/gamepup-game.service
visudo -cf /etc/sudoers.d/gamepup-rom-import
if id "$DEVICE_USER" >/dev/null 2>&1; then
	install -d -o "$DEVICE_USER" -g "$DEVICE_USER" -m 0755 \
		/opt/gamepup/games/nes /opt/gamepup/games/gbc \
		/opt/gamepup/games/n64 /opt/gamepup/games/doom
	chown "$DEVICE_USER:$DEVICE_USER" /opt/gamepup/saves /opt/gamepup/gifs
fi
/usr/local/libexec/gamepup-rom-drive-setup
systemctl daemon-reload
systemctl enable gamepup-oled-status.service
systemctl enable gamepup-rom-import-watch.service
systemctl enable gamepup-game.service

if ! grep -qF "$OVERLAY_TARGET" "$EXTLINUX_CONFIG"; then
	if [ ! -e "$EXTLINUX_CONFIG.before-gamepup-a4" ]; then
		cp -a "$EXTLINUX_CONFIG" "$EXTLINUX_CONFIG.before-gamepup-a4"
	fi
	sed -i "/^[[:space:]]*fdt[[:space:]]/a\\  fdtoverlays /dtb/ti/$OVERLAY_NAME.dtbo" \
		"$EXTLINUX_CONFIG"
fi

modprobe drm_mipi_dbi
modprobe st7735r

echo "GamePup A4 support installed for kernel $KERNEL_VERSION."
echo "Reboot to apply the overlay."
