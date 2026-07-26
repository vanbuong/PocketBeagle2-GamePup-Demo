#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Install the complete GamePup demo, excluding the optional N64 core.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
INSTALL_DOOM=${GAMEPUP_INSTALL_DOOM:-1}
INSTALL_GPU=${GAMEPUP_INSTALL_GPU:-1}
MODEL=$(tr '\000' ' ' </proc/device-tree/model 2>/dev/null || true)

if [ "$(id -u)" -ne 0 ]; then
	echo "Run this installer as root." >&2
	exit 1
fi

case "$(uname -m)" in
aarch64|arm64) ;;
*)
	echo "This installer requires 64-bit Arm Linux." >&2
	exit 1
	;;
esac

case "$MODEL" in
*PocketBeagle2*|*"PocketBeagle 2"*) ;;
*)
	if [ "${GAMEPUP_ALLOW_UNSUPPORTED:-0}" != 1 ]; then
		echo "Expected a PocketBeagle 2; detected: ${MODEL:-unknown}." >&2
		echo "Set GAMEPUP_ALLOW_UNSUPPORTED=1 only for development." >&2
		exit 1
	fi
	;;
esac

echo "[1/4] Building the pinned NES and Game Boy libretro cores..."
"$SCRIPT_DIR/emulator/install-cores.sh"

echo "[2/4] Installing the GamePup cape support and application..."
"$SCRIPT_DIR/install.sh"

if [ "$INSTALL_DOOM" = 1 ]; then
	echo "[3/4] Installing PrBoom and the separately licensed Doom shareware data..."
	"$SCRIPT_DIR/emulator/install-doom.sh"
else
	echo "[3/4] Skipping Doom (GAMEPUP_INSTALL_DOOM=$INSTALL_DOOM)."
fi

if [ "$INSTALL_GPU" = 1 ]; then
	echo "[4/4] Installing the TI PowerVR stack and GPU demonstrations..."
	"$SCRIPT_DIR/emulator/install-gpu.sh"
else
	echo "[4/4] Skipping the TI PowerVR stack (GAMEPUP_INSTALL_GPU=$INSTALL_GPU)."
fi

echo
echo "GamePup installation is complete. Reboot the PocketBeagle 2 to activate it."
echo "Nintendo 64 remains optional; see the README for the Docker build step."
