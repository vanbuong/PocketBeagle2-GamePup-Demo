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
MODULE_SOURCE_DIR=/usr/src/gamepup-ili9341-$KERNEL_VERSION
MODULE_INSTALL_DIR=/lib/modules/$KERNEL_VERSION/updates/gamepup
# Split overlays: LCD on SPI0, audio/controls, Eth Wiz on SPI2.
OVERLAY_NAMES="k3-am62-pocketbeagle2-spi0-ili9341
k3-am62-pocketbeagle2-gamepup-audio
k3-am62-pocketbeagle2-spi2-eth-wiz-click"
EXTLINUX_CONFIG=
for candidate in /boot/firmware/extlinux/extlinux.conf \
	/boot/extlinux/extlinux.conf; do
	if [ -f "$candidate" ]; then
		EXTLINUX_CONFIG=$candidate
		break
	fi
done
# Prefer an existing login account. Stock Armbian uses "beagle"; some images
# use another UID>=1000 name (e.g. buongvv). Override with DEVICE_USER=.
resolve_device_user() {
	if [ -n "${DEVICE_USER:-}" ]; then
		printf '%s\n' "$DEVICE_USER"
		return 0
	fi
	# Who ran sudo ./install.sh — usually the board login (e.g. buongvv).
	if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != root ] &&
		id "$SUDO_USER" >/dev/null 2>&1; then
		printf '%s\n' "$SUDO_USER"
		return 0
	fi
	for candidate in beagle debian ubuntu; do
		if id "$candidate" >/dev/null 2>&1; then
			printf '%s\n' "$candidate"
			return 0
		fi
	done
	# First non-system login user (uid >= 1000), excluding nobody.
	awk -F: '$3 >= 1000 && $1 != "nobody" { print $1; exit }' /etc/passwd
}

DEVICE_USER=$(resolve_device_user)
if [ -z "$DEVICE_USER" ] || ! id "$DEVICE_USER" >/dev/null 2>&1; then
	echo "No DEVICE_USER found. Set DEVICE_USER=yourlogin and re-run." >&2
	exit 1
fi
echo "GamePup service user: $DEVICE_USER"

resolve_overlay_dir() {
	for d in /boot/firmware/overlays /boot/overlays /boot/dtb/ti \
		/boot/firmware/ti; do
		if [ -d "$d" ]; then
			printf '%s\n' "$d"
			return 0
		fi
	done
	install -d -m 0755 /boot/dtb/ti
	printf '%s\n' /boot/dtb/ti
}

overlay_extlinux_path() {
	abs=$1
	case "$abs" in
		/boot/firmware/*) printf '/%s\n' "${abs#/boot/firmware/}" ;;
		/boot/*) printf '/%s\n' "${abs#/boot/}" ;;
		*) printf '%s\n' "$abs" ;;
	esac
}

missing_packages=
for package in build-essential ca-certificates curl device-tree-compiler \
	dosfstools gcc-14 libgif-dev mpv alsa-utils; do
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
curl -fsSLo "$MODULE_SOURCE_DIR/ili9341.c" \
	"https://raw.githubusercontent.com/gregkh/linux/$UPSTREAM_VERSION/drivers/gpu/drm/tiny/ili9341.c"

make -C "/lib/modules/$KERNEL_VERSION/build" M="$MODULE_SOURCE_DIR" \
	CC="$KERNEL_CC" modules
install -m 0644 "$MODULE_SOURCE_DIR/drm_mipi_dbi.ko" "$MODULE_INSTALL_DIR/"
install -m 0644 "$MODULE_SOURCE_DIR/ili9341.ko" "$MODULE_INSTALL_DIR/"
depmod -a "$KERNEL_VERSION"

OVERLAY_DIR=$(resolve_overlay_dir)
install -d -m 0755 "$OVERLAY_DIR"
fdtoverlays_args=
for OVERLAY_NAME in $OVERLAY_NAMES; do
	src="$SCRIPT_DIR/overlays/$OVERLAY_NAME.dts"
	dst="$OVERLAY_DIR/$OVERLAY_NAME.dtbo"
	[ -f "$src" ] || {
		echo "Missing overlay source: $src" >&2
		exit 1
	}
	dtc -@ -I dts -O dtb -o "$dst" "$src"
	rel=$(overlay_extlinux_path "$dst")
	fdtoverlays_args="$fdtoverlays_args $rel"
	# Mirror into /boot/dtbs/<kver>/ti when present (Armbian).
	if [ -d "/boot/dtbs/$KERNEL_VERSION/ti" ]; then
		install -m 0644 "$dst" \
			"/boot/dtbs/$KERNEL_VERSION/ti/$OVERLAY_NAME.dtbo"
	fi
	echo "Built overlay: $dst"
done

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
udevadm trigger --subsystem-match=i2c-dev
udevadm trigger --subsystem-match=input

install -d -m 0755 /usr/local/bin /usr/local/libexec /usr/local/sbin
# Khronos headers are vendored under emulator/khronos so we never need Mesa
# libegl-dev (it Conflicts with TI libegl-mesa0-pvr). Link against whatever
# libEGL is present; if none, install Mesa runtime only when TI PVR is absent.
egl_link_ok=0
if echo 'int main(void){return 0;}' | gcc -x c - -lEGL -lGLESv2 \
	-o /tmp/gamepup-egl-check 2>/dev/null; then
	egl_link_ok=1
	rm -f /tmp/gamepup-egl-check
elif ! dpkg-query -W -f='${db:Status-Abbrev}' ti-img-rogue-umlibs-am62 \
	2>/dev/null | grep -q '^ii'; then
	apt-get install -y --no-install-recommends libegl1 libgles2 || true
	if echo 'int main(void){return 0;}' | gcc -x c - -lEGL -lGLESv2 \
		-o /tmp/gamepup-egl-check 2>/dev/null; then
		egl_link_ok=1
		rm -f /tmp/gamepup-egl-check
	fi
fi
make -C "$SCRIPT_DIR/emulator" clean
if [ "$egl_link_ok" = 1 ] && make -C "$SCRIPT_DIR/emulator" all; then
	:
elif make -C "$SCRIPT_DIR/emulator" apps; then
	echo "Built without GLES apps; run emulator/install-gpu.sh to install" \
		"TI PowerVR and build gamepup-retro / gamepup-gpu-bench." >&2
else
	echo "Failed to build GamePup userspace apps." >&2
	exit 1
fi
if [ -e "$SCRIPT_DIR/emulator/gamepup-retro" ]; then
	install -m 0755 "$SCRIPT_DIR/emulator/gamepup-retro" \
		/usr/local/bin/gamepup-retro
fi
install -m 0755 "$SCRIPT_DIR/emulator/gamepup-oled-status" \
	/usr/local/bin/gamepup-oled-status
if [ -e "$SCRIPT_DIR/emulator/gamepup-gpu-bench" ]; then
	install -m 0755 "$SCRIPT_DIR/emulator/gamepup-gpu-bench" \
		/usr/local/bin/gamepup-gpu-bench
fi
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
install -m 0644 "$SCRIPT_DIR/emulator/pb2-usb-gadget.service" \
	/etc/systemd/system/pb2-usb-gadget.service
install -d -m 0755 /usr/local/share/gamepup/rom-drive /opt/gamepup/saves
install -m 0644 "$SCRIPT_DIR/emulator/rom-drive/README.txt" \
	/usr/local/share/gamepup/rom-drive/README.txt
# Rewrite hardcoded "beagle" to the board login account.
sed "s/^beagle /$DEVICE_USER /" \
	"$SCRIPT_DIR/emulator/gamepup-rom-import.sudoers" \
	> /etc/sudoers.d/gamepup-rom-import
chmod 0440 /etc/sudoers.d/gamepup-rom-import
install -m 0644 "$SCRIPT_DIR/emulator/gamepup-rom-import-watch.service" \
	/etc/systemd/system/gamepup-rom-import-watch.service
sed -e "s/^User=beagle$/User=$DEVICE_USER/" \
	-e "s/^Group=beagle$/Group=$DEVICE_USER/" \
	"$SCRIPT_DIR/emulator/gamepup-game.service" \
	> /etc/systemd/system/gamepup-game.service
chmod 0644 /etc/systemd/system/gamepup-game.service
visudo -cf /etc/sudoers.d/gamepup-rom-import
install -d -o "$DEVICE_USER" -g "$DEVICE_USER" -m 0755 \
	/opt/gamepup /opt/gamepup/games/nes /opt/gamepup/games/gbc \
	/opt/gamepup/games/n64 /opt/gamepup/games/doom \
	/opt/gamepup/saves /opt/gamepup/gifs \
	/opt/gamepup/voice-memos /opt/gamepup/music
# Menu writes /opt/gamepup/selected-rom as the service user.
touch /opt/gamepup/selected-rom
chown -R "$DEVICE_USER:$DEVICE_USER" /opt/gamepup
usermod -a -G video,render,input,spi,i2c "$DEVICE_USER" 2>/dev/null || \
	usermod -a -G video,input,spi,i2c "$DEVICE_USER" 2>/dev/null || true
/usr/local/libexec/gamepup-rom-drive-setup
systemctl daemon-reload
# Stock BeagleBoard NCM-only gadgets claim the USB device controller; GamePup
# needs the same UDC for NCM + ACM + the GAMEPUP mass-storage inbox.
for _gadget_unit in bb-usb-gadgets.service usb-gadget.service gadget-init.service; do
	systemctl disable --now "$_gadget_unit" 2>/dev/null || true
	systemctl mask "$_gadget_unit" 2>/dev/null || true
done
systemctl enable --now pb2-usb-gadget.service
systemctl enable gamepup-oled-status.service
systemctl enable --now gamepup-rom-import-watch.service
systemctl enable gamepup-game.service

if [ -n "$EXTLINUX_CONFIG" ]; then
	if [ ! -e "$EXTLINUX_CONFIG.before-gamepup-a4" ]; then
		cp -a "$EXTLINUX_CONFIG" "$EXTLINUX_CONFIG.before-gamepup-a4"
	fi
	# Drop prior GamePup / split-overlay fdtoverlays lines, then add the set.
	sed -i \
		-e '/^[[:space:]]*fdtoverlays[[:space:]].*gamepup/d' \
		-e '/^[[:space:]]*fdtoverlays[[:space:]].*k3-am62-pocketbeagle2-spi0-ili9341/d' \
		-e '/^[[:space:]]*fdtoverlays[[:space:]].*k3-am62-pocketbeagle2-gamepup-audio/d' \
		-e '/^[[:space:]]*fdtoverlays[[:space:]].*k3-am62-pocketbeagle2-spi2-eth-wiz/d' \
		-e '/^[[:space:]]*fdtoverlays[[:space:]].*k3-am6232-pocketbeagle2-gamepup-a4/d' \
		"$EXTLINUX_CONFIG"
	if grep -q '^[[:space:]]*fdt[[:space:]]' "$EXTLINUX_CONFIG"; then
		sed -i "/^[[:space:]]*fdt[[:space:]]/a\\  fdtoverlays$fdtoverlays_args" \
			"$EXTLINUX_CONFIG"
	else
		printf '  fdtoverlays%s\n' "$fdtoverlays_args" >> "$EXTLINUX_CONFIG"
	fi
	echo "Updated $EXTLINUX_CONFIG with:$fdtoverlays_args"
else
	echo "No extlinux.conf found; install dtbos manually from $OVERLAY_DIR" >&2
fi

install -d -m 0755 /etc/modules-load.d
install -m 0644 "$SCRIPT_DIR/emulator/gamepup-alsa.conf" \
	/etc/modules-load.d/gamepup-alsa.conf
modprobe snd-soc-davinci-mcasp 2>/dev/null || true
modprobe snd-soc-max98357a 2>/dev/null || true
modprobe snd-soc-simple-card 2>/dev/null || true

modprobe drm_mipi_dbi
modprobe ili9341

echo "GamePup support installed for kernel $KERNEL_VERSION."
echo "Overlays: SPI0 ILI9341 + audio/controls + SPI2 Eth Wiz."
echo "Reboot to apply the overlays."
