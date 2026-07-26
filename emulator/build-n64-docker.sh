#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CORE_COMMIT=98c1b0d877542b01314b3b04272282ba223b65b3
OUTPUT_DIR=${1:-$SCRIPT_DIR/build/n64}
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/gamepup-n64.XXXXXX")

cleanup()
{
	rm -rf "$BUILD_DIR"
}
trap cleanup EXIT HUP INT TERM

git clone --filter=blob:none https://github.com/libretro/mupen64plus-libretro-nx.git \
	"$BUILD_DIR/source"
git -C "$BUILD_DIR/source" checkout "$CORE_COMMIT"

docker run --rm --platform linux/arm64 \
	-v "$BUILD_DIR/source:/src" -w /src ubuntu:24.04 sh -ec '
		export DEBIAN_FRONTEND=noninteractive
		apt-get update -qq
		apt-get install -y -qq --no-install-recommends \
			build-essential ca-certificates git libgles2-mesa-dev
		make -j"$(nproc)" platform=arm64_cortex_a53_gles3
	'

mkdir -p "$OUTPUT_DIR"
install -m 0755 "$BUILD_DIR/source/mupen64plus_next_libretro.so" \
	"$OUTPUT_DIR/mupen64plus_next_libretro.so"
printf 'Built %s\n' "$OUTPUT_DIR/mupen64plus_next_libretro.so"
