#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "Run this installer as root." >&2
	exit 1
fi

PRBOOM_COMMIT=b4dfa039e4192dd392a2ed49864bed7c65f171e7
DEVICE_USER=${DEVICE_USER:-beagle}
BUILD_JOBS=${BUILD_JOBS:-2}
BUILD_DIR=$(mktemp -d /tmp/gamepup-prboom.XXXXXX)

cleanup() {
	case "$BUILD_DIR" in
	/tmp/gamepup-prboom.*) rm -rf -- "$BUILD_DIR" ;;
	esac
}
trap cleanup EXIT HUP INT TERM

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
	doom-wad-shareware gcc git make

mkdir "$BUILD_DIR/source"
git -C "$BUILD_DIR/source" init -q
git -C "$BUILD_DIR/source" remote add origin \
	https://github.com/libretro/libretro-prboom.git
git -C "$BUILD_DIR/source" fetch -q --depth 1 origin "$PRBOOM_COMMIT"
git -C "$BUILD_DIR/source" checkout -q --detach FETCH_HEAD
make -C "$BUILD_DIR/source" -j"$BUILD_JOBS"

install -d -m 0755 /usr/local/lib/libretro /opt/gamepup/games/doom
install -m 0755 "$BUILD_DIR/source/prboom_libretro.so" \
	/usr/local/lib/libretro/prboom_libretro.so
install -m 0644 /usr/share/games/doom/doom1.wad \
	"/opt/gamepup/games/doom/Doom Shareware.wad"
chown "$DEVICE_USER:$DEVICE_USER" /opt/gamepup/games/doom \
	"/opt/gamepup/games/doom/Doom Shareware.wad"

echo "PrBoom and Doom Shareware installed."
