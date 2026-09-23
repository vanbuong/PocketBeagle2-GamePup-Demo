#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Install artifacts produced by scripts/cross-build.sh onto a PocketBeagle 2.
#
# Usage:
#   sudo ./scripts/install-dist.sh ./dist

set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "Run as root." >&2
	exit 1
fi

DIST_DIR=${1:-./dist}
DEVICE_USER=${DEVICE_USER:-beagle}
KERNEL_VERSION=$(uname -r)
OVERLAY_NAME=k3-am6232-pocketbeagle2-gamepup-a4
OVERLAY_TARGET=/boot/dtb/ti/$OVERLAY_NAME.dtbo
EXTLINUX_CONFIG=/boot/extlinux/extlinux.conf
MODULE_INSTALL_DIR=/lib/modules/$KERNEL_VERSION/updates/gamepup

[ -d "$DIST_DIR/bin" ] || {
	echo "Missing $DIST_DIR/bin — pass the cross-build dist directory." >&2
	exit 1
}

if [ -f "$DIST_DIR/modules/KERNEL_VERSION.txt" ]; then
	built_for=$(cat "$DIST_DIR/modules/KERNEL_VERSION.txt")
	if [ "$built_for" != "$KERNEL_VERSION" ]; then
		echo "Warning: modules were built for $built_for, running kernel is $KERNEL_VERSION." >&2
		echo "Continue only if those versions are ABI-compatible." >&2
	fi
fi

install -d -m 0755 /usr/local/bin /usr/local/include /usr/local/lib/libretro \
	/usr/local/share/gamepup/bezels /opt/gamepup/gifs /opt/gamepup/saves \
	/opt/gamepup/games/nes /opt/gamepup/games/gbc /opt/gamepup/games/n64 \
	/opt/gamepup/games/doom /opt/gamepup/voice-memos /opt/gamepup/music \
	"$MODULE_INSTALL_DIR"

install -m 0755 "$DIST_DIR/bin/"* /usr/local/bin/
if [ -f "$DIST_DIR/include/libretro.h" ]; then
	install -m 0644 "$DIST_DIR/include/libretro.h" /usr/local/include/
fi
if [ -d "$DIST_DIR/libretro" ]; then
	install -m 0755 "$DIST_DIR/libretro/"*.so /usr/local/lib/libretro/
fi
if [ -d "$DIST_DIR/share/bezels" ]; then
	install -m 0644 "$DIST_DIR/share/bezels/"*-system.rgb \
		/usr/local/share/gamepup/bezels/ 2>/dev/null || true
fi
if [ -d "$DIST_DIR/share/gifs" ]; then
	install -m 0644 "$DIST_DIR/share/gifs/"*.gif /opt/gamepup/gifs/ 2>/dev/null || true
fi

if [ -f "$DIST_DIR/etc/modules-load.d/gamepup-alsa.conf" ]; then
	install -d -m 0755 /etc/modules-load.d
	install -m 0644 "$DIST_DIR/etc/modules-load.d/gamepup-alsa.conf" \
		/etc/modules-load.d/gamepup-alsa.conf
	modprobe snd-soc-davinci-mcasp 2>/dev/null || true
	modprobe snd-soc-max98357a 2>/dev/null || true
	modprobe snd-soc-audio-graph-card 2>/dev/null || true
	# Re-bind if the card probed before both DAIs were registered.
	if [ -e /sys/bus/platform/devices/sound-gamepup ]; then
		echo sound-gamepup > /sys/bus/platform/drivers/asoc-audio-graph-card/unbind 2>/dev/null || true
		echo sound-gamepup > /sys/bus/platform/drivers/asoc-audio-graph-card/bind 2>/dev/null || true
		echo sound-gamepup > /sys/bus/platform/drivers/asoc-simple-card/unbind 2>/dev/null || true
		echo sound-gamepup > /sys/bus/platform/drivers/asoc-simple-card/bind 2>/dev/null || true
	fi
fi

# Doom shareware WAD (same as emulator/install-doom.sh) and ALSA runtime
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends libasound2t64 || \
	apt-get install -y --no-install-recommends libasound2
apt-get install -y --no-install-recommends alsa-utils mpv
if [ ! -f /usr/share/games/doom/doom1.wad ]; then
	apt-get install -y --no-install-recommends doom-wad-shareware
fi
install -m 0644 /usr/share/games/doom/doom1.wad \
	"/opt/gamepup/games/doom/Doom Shareware.wad"

if [ -d "$DIST_DIR/modules" ]; then
	install -m 0644 "$DIST_DIR/modules/"*.ko "$MODULE_INSTALL_DIR/"
	depmod -a "$KERNEL_VERSION"
	modprobe drm_mipi_dbi || true
	modprobe ili9341 || true
fi

if [ -f "$DIST_DIR/dtbo/$OVERLAY_NAME.dtbo" ]; then
	install -d -m 0755 /boot/dtb/ti
	install -m 0644 "$DIST_DIR/dtbo/$OVERLAY_NAME.dtbo" "$OVERLAY_TARGET"
	# Some images also keep a copy under /boot/dtbs/<kver>/ti/
	if [ -d "/boot/dtbs/$KERNEL_VERSION/ti" ]; then
		install -m 0644 "$DIST_DIR/dtbo/$OVERLAY_NAME.dtbo" \
			"/boot/dtbs/$KERNEL_VERSION/ti/$OVERLAY_NAME.dtbo"
	fi
	if [ -f "$EXTLINUX_CONFIG" ] && ! grep -qF "$OVERLAY_NAME.dtbo" "$EXTLINUX_CONFIG"; then
		if [ ! -e "$EXTLINUX_CONFIG.before-gamepup-a4" ]; then
			cp -a "$EXTLINUX_CONFIG" "$EXTLINUX_CONFIG.before-gamepup-a4"
		fi
		sed -i "/^[[:space:]]*fdt[[:space:]]/a\\  fdtoverlays /dtb/ti/$OVERLAY_NAME.dtbo" \
			"$EXTLINUX_CONFIG"
	fi
	echo "Installed overlay: $OVERLAY_TARGET"
	echo "After reboot verify audio DT with:"
	echo "  cat /proc/device-tree/chosen/overlays/gamepup-a4-audio"
	echo "  ls /proc/device-tree/sound-gamepup"
	echo "  ls /proc/device-tree/gamepup-max98357a"
	echo "  cat /proc/device-tree/bus@f0000/audio-controller@2b20000/status"
fi

if id "$DEVICE_USER" >/dev/null 2>&1; then
	chown -R "$DEVICE_USER:$DEVICE_USER" /opt/gamepup/gifs /opt/gamepup/saves \
		/opt/gamepup/games /opt/gamepup/voice-memos /opt/gamepup/music \
		2>/dev/null || true
	chown "$DEVICE_USER:$DEVICE_USER" /opt/gamepup/games/doom \
		"/opt/gamepup/games/doom/Doom Shareware.wad"
fi

echo "Installed cross-build artifacts from $DIST_DIR."
echo "Doom Shareware WAD: /opt/gamepup/games/doom/Doom Shareware.wad"
echo "Voice memos: /opt/gamepup/voice-memos"
echo "Music library: /opt/gamepup/music"
echo "Reboot to apply the device-tree overlay if it was newly added."
