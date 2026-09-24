#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "Run this installer as root." >&2
	exit 1
fi

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
KERNEL_VERSION=$(uname -r)
KERNEL_CC=${KERNEL_CC:-gcc-14}
export KERNEL_CC
TEMP_DIR=$(mktemp -d)
trap 'find "$TEMP_DIR" -depth -delete' EXIT HUP INT TERM

curl -fsSLo "$TEMP_DIR/ti-debpkgs.sources" \
	https://raw.githubusercontent.com/TexasInstruments/ti-debpkgs/main/ti-debpkgs.sources
sed -i 's/^Suites:.*/Suites: noble/' "$TEMP_DIR/ti-debpkgs.sources"
curl -fsSLo "$TEMP_DIR/ti-debpkgs.pref" \
	https://raw.githubusercontent.com/armbian/build/main/packages/bsp/ti/ti-debpkgs/ti-debpkgs
install -m 0644 "$TEMP_DIR/ti-debpkgs.sources" \
	/etc/apt/sources.list.d/ti-debpkgs.sources
install -m 0644 "$TEMP_DIR/ti-debpkgs.pref" \
	/etc/apt/preferences.d/ti-debpkgs
install -m 0644 "$SCRIPT_DIR/gamepup-ti-dkms.conf" \
	/etc/dkms/ti-img-rogue-driver.conf

apt-get update

# Mesa's libegl-dev / libgles-dev Depend on libegl1 / libgles2, which conflict
# with TI's libegl-mesa0-pvr / libgles2-mesa-pvr. Headers come from
# emulator/khronos/; runtime libs come from ti-img-rogue-umlibs-am62.
if dpkg-query -W -f='${db:Status-Abbrev}' libegl-dev 2>/dev/null | grep -q '^ii'; then
	echo "Removing Mesa libegl-dev (conflicts with TI PowerVR EGL)..."
	apt-get remove -y libegl-dev libgles-dev \
		libegl1-mesa-dev libgles2-mesa-dev 2>/dev/null || true
fi
if dpkg-query -W -f='${db:Status-Abbrev}' libegl1 2>/dev/null | grep -q '^ii'; then
	echo "Removing Mesa libegl1 / libgles2 (conflicts with TI PowerVR EGL)..."
	apt-get remove -y libegl1 libgles2 2>/dev/null || true
fi

apt-get install -y --no-install-recommends gcc-14 dkms \
	libvulkan-dev vulkan-tools ti-img-rogue-driver-am62-dkms \
	ti-img-rogue-umlibs-am62 ti-img-rogue-tools-am62 \
	ti-img-rogue-firmware-am62

dkms autoinstall -k "$KERNEL_VERSION"

install -m 0644 "$SCRIPT_DIR/gamepup-powervr-blacklist.conf" \
	/etc/modprobe.d/gamepup-powervr.conf
install -m 0644 "$SCRIPT_DIR/gamepup-powervr-modules.conf" \
	/etc/modules-load.d/gamepup-powervr.conf

# Rebuild GLES-linked apps against TI's libEGL / libGLESv2.
make -C "$SCRIPT_DIR" clean
make -C "$SCRIPT_DIR" gles
install -m 0755 "$SCRIPT_DIR/gamepup-gpu-bench" /usr/local/bin/gamepup-gpu-bench
install -m 0755 "$SCRIPT_DIR/gamepup-retro" /usr/local/bin/gamepup-retro

echo "TI PowerVR packages and GamePup GPU apps installed."
echo "Reboot to switch from the upstream powervr module to TI pvrsrvkm."
