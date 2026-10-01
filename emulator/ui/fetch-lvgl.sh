#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Fetch the pinned LVGL release used by gamepup-ui into emulator/ui/lvgl.
set -eu
LVGL_TAG=${LVGL_TAG:-v9.2.2}
DEST=${1:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lvgl}
if [ -f "$DEST/lvgl.h" ]; then
	exit 0
fi
rm -rf "$DEST"
git clone --depth 1 --branch "$LVGL_TAG" https://github.com/lvgl/lvgl.git "$DEST"
