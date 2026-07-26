#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build reproducible ARM64 NES and Game Boy libretro cores on the device.

set -eu

NESTOPIA_COMMIT=b0fd87dd07e3c52903435d302b04e5e97796f127
GAMBATTE_COMMIT=9b3b5e3cc18ec92f460d37dd551eaf90c55bfcea
BUILD_JOBS=${BUILD_JOBS:-2}
BUILD_DIR=$(mktemp -d /tmp/gamepup-cores.XXXXXX)
NESTOPIA_DIR=$BUILD_DIR/nestopia
GAMBATTE_DIR=$BUILD_DIR/gambatte

cleanup()
{
	case "$BUILD_DIR" in
	/tmp/gamepup-cores.*) find "$BUILD_DIR" -depth -delete ;;
	esac
}
trap cleanup EXIT HUP INT TERM

if [ "$(id -u)" -ne 0 ]; then
	echo "Run this installer as root." >&2
	exit 1
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
	build-essential ca-certificates git

fetch_commit()
{
	repository=$1
	commit=$2
	destination=$3

	mkdir "$destination"
	git -C "$destination" init -q
	git -C "$destination" remote add origin "$repository"
	git -C "$destination" fetch -q --depth 1 origin "$commit"
	git -C "$destination" checkout -q --detach FETCH_HEAD
}

fetch_commit https://github.com/libretro/nestopia.git \
	"$NESTOPIA_COMMIT" "$NESTOPIA_DIR"
fetch_commit https://github.com/libretro/gambatte-libretro.git \
	"$GAMBATTE_COMMIT" "$GAMBATTE_DIR"

make -C "$NESTOPIA_DIR/libretro" -j"$BUILD_JOBS" platform=unix
make -C "$GAMBATTE_DIR" -f Makefile.libretro \
	-j"$BUILD_JOBS" platform=unix

install -d -m 0755 /usr/local/include /usr/local/lib/libretro
install -m 0644 \
	"$NESTOPIA_DIR/libretro/libretro-common/include/libretro.h" \
	/usr/local/include/libretro.h
install -m 0755 "$NESTOPIA_DIR/libretro/nestopia_libretro.so" \
	/usr/local/lib/libretro/nestopia_libretro.so
install -m 0755 "$GAMBATTE_DIR/gambatte_libretro.so" \
	/usr/local/lib/libretro/gambatte_libretro.so

echo "Pinned Nestopia and Gambatte cores installed."
