#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Install cross-build / CI userspace artifacts onto a PocketBeagle 2.
# Skips kernel modules and the device-tree overlay (use install.sh / install-dist.sh
# for those).
#
# Usage:
#   sudo ./scripts/install-artifacts.sh ./dist
#   sudo ./scripts/install-artifacts.sh /path/to/gamepup-pb2-debian-13.7-aarch64

set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "Run as root." >&2
	exit 1
fi

DIST_DIR=${1:-./dist}
DEVICE_USER=${DEVICE_USER:-beagle}

[ -d "$DIST_DIR/bin" ] || {
	echo "Missing $DIST_DIR/bin — pass the extracted CI/cross-build dist directory." >&2
	exit 1
}

install -d -m 0755 /usr/local/bin /usr/local/include /usr/local/lib/libretro \
	/usr/local/share/gamepup/bezels /opt/gamepup/gifs /opt/gamepup/saves \
	/opt/gamepup/games/nes /opt/gamepup/games/gbc /opt/gamepup/games/n64 \
	/opt/gamepup/games/doom /opt/gamepup/voice-memos /opt/gamepup/music

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

if id "$DEVICE_USER" >/dev/null 2>&1; then
	chown -R "$DEVICE_USER:$DEVICE_USER" /opt/gamepup/gifs /opt/gamepup/saves \
		/opt/gamepup/games /opt/gamepup/voice-memos /opt/gamepup/music \
		2>/dev/null || true
	chown "$DEVICE_USER:$DEVICE_USER" /opt/gamepup/games/doom \
		"/opt/gamepup/games/doom/Doom Shareware.wad"
fi

echo "Installed userspace artifacts from $DIST_DIR:"
echo "  /usr/local/bin          (menu, retro, gpu-bench, hardware-test, voice-memo, music-player, ...)"
echo "  /usr/local/lib/libretro (cores, if present)"
echo "  /usr/local/share/gamepup/bezels"
echo "  /opt/gamepup/gifs"
echo "  /opt/gamepup/voice-memos"
echo "  /opt/gamepup/music"
echo "  /opt/gamepup/games/doom/Doom Shareware.wad"
echo "Skipped: kernel modules and device-tree overlay."
