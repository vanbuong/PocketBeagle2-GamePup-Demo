#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Cross-compile PocketBeagle 2 GamePup artifacts for Debian 13.7 IoT (v6.18.x-k3).
#
# Produces dist/ containing:
#   dtbo/, modules/, bin/, libretro/, include/, MANIFEST.txt
#
# Usage:
#   ./scripts/cross-build.sh
#   SKIP_N64=1 ./scripts/cross-build.sh
#   SKIP_MODULES=1 ./scripts/cross-build.sh

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
# shellcheck disable=SC1091
. "$ROOT_DIR/ci/target.env"

BUILD_JOBS=${BUILD_JOBS:-$(nproc 2>/dev/null || echo 2)}
# BUILD_N64 defaults on; SKIP_N64=1 disables it. BUILD_N64=0 also disables.
BUILD_N64=${BUILD_N64:-1}
SKIP_N64=${SKIP_N64:-0}
SKIP_MODULES=${SKIP_MODULES:-0}
SKIP_CORES=${SKIP_CORES:-0}
DIST_DIR=${DIST_DIR:-$ROOT_DIR/dist}
WORK_DIR=${WORK_DIR:-$ROOT_DIR/.cross-build}
HOST_ARCH=$(uname -m)
CROSS_TRIPLE=${CROSS_TRIPLE:-aarch64-linux-gnu}
CROSS_GCC=${CROSS_GCC:-}
TARGET_ARCH=arm64

log() {
	printf '==> %s\n' "$*"
}

die() {
	printf 'error: %s\n' "$*" >&2
	exit 1
}

require_cmd() {
	command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

pick_cross_gcc() {
	if [ -n "$CROSS_GCC" ]; then
		command -v "$CROSS_GCC" >/dev/null 2>&1 || die "CROSS_GCC not found: $CROSS_GCC"
	else
		for candidate in "${CROSS_TRIPLE}-gcc-14" "${CROSS_TRIPLE}-gcc"; do
			if command -v "$candidate" >/dev/null 2>&1; then
				CROSS_GCC=$candidate
				break
			fi
		done
		[ -n "$CROSS_GCC" ] || die "install ${CROSS_TRIPLE}-gcc-14 (or set CROSS_GCC)"
	fi
	CROSS_CXX=$(printf '%s\n' "$CROSS_GCC" | sed 's/gcc/g++/g')
	command -v "$CROSS_CXX" >/dev/null 2>&1 || CROSS_CXX=$CROSS_GCC
}

resolve_headers_package() {
	packages_gz=$WORK_DIR/Packages.gz
	curl -fsSL "$BB_REPO_URL/dists/$BB_REPO_DIST/main/binary-arm64/Packages.gz" \
		-o "$packages_gz"
	meta_block=$(gzip -dc "$packages_gz" | awk -v pkg="$BB_HEADERS_META" '
		$0 == "Package: " pkg {p=1}
		p {print}
		p && NF == 0 {exit}
	')
	pre_depends=$(printf '%s\n' "$meta_block" | awk -F': ' '
		/^Pre-Depends:/ {
			gsub(/,.*/, "", $2)
			gsub(/^ +| +$/, "", $2)
			print $2
			exit
		}
	')
	if [ -z "$pre_depends" ]; then
		log "meta package $BB_HEADERS_META unresolved; using fallback $BB_HEADERS_PACKAGE_FALLBACK"
		HEADERS_PACKAGE=$BB_HEADERS_PACKAGE_FALLBACK
		HEADERS_VERSION=$BB_HEADERS_VERSION_FALLBACK
	else
		HEADERS_PACKAGE=$pre_depends
		HEADERS_VERSION=${HEADERS_PACKAGE#linux-headers-}
	fi

	headers_block=$(gzip -dc "$packages_gz" | awk -v pkg="$HEADERS_PACKAGE" '
		$0 == "Package: " pkg {p=1}
		p {print}
		p && NF == 0 {exit}
	')
	HEADERS_FILENAME=$(printf '%s\n' "$headers_block" | awk -F': ' '
		/^Filename:/ {print $2; exit}
	')
	HEADERS_SHA256=$(printf '%s\n' "$headers_block" | awk -F': ' '
		/^SHA256:/ {print $2; exit}
	')
	[ -n "$HEADERS_FILENAME" ] || die "could not resolve Filename for $HEADERS_PACKAGE"
	log "Using kernel headers $HEADERS_PACKAGE"
}

fetch_headers() {
	resolve_headers_package
	deb=$WORK_DIR/headers.deb
	curl -fsSL "$BB_REPO_URL/$HEADERS_FILENAME" -o "$deb"
	if [ -n "$HEADERS_SHA256" ]; then
		echo "$HEADERS_SHA256  $deb" | sha256sum -c -
	fi
	rm -rf "$WORK_DIR/headers-root"
	mkdir -p "$WORK_DIR/headers-root"
	dpkg-deb -x "$deb" "$WORK_DIR/headers-root"
	KDIR=$WORK_DIR/headers-root/usr/src/linux-headers-$HEADERS_VERSION
	[ -d "$KDIR" ] || die "headers tree missing at $KDIR"
	if [ ! -f "$KDIR/.config" ] && [ -f "$KDIR/include/config/auto.conf" ]; then
		cp "$KDIR/include/config/auto.conf" "$KDIR/.config"
	fi
}

wrap_aarch64_host_tools() {
	# BeagleBoard headers ship aarch64 host helpers. On x86_64 CI hosts, wrap
	# them with qemu-aarch64-static so out-of-tree module builds can cross.
	kdir=$1
	qemu=$(command -v qemu-aarch64-static || true)
	[ -n "$qemu" ] || die "qemu-aarch64-static required to cross-build modules on $HOST_ARCH"
	find "$kdir/scripts" -type f -executable | while read -r bin; do
		case $(file -b "$bin") in
		*ARM\ aarch64*)
			mv "$bin" "$bin.real"
			printf '#!/bin/sh\nexec %s %s.real "$@"\n' "$qemu" "$bin" >"$bin"
			chmod +x "$bin"
			;;
		esac
	done
}

fetch_driver_sources() {
	module_dir=$1
	# Prefer the exact stable tag matching the headers version; fall back to
	# the upstream v6.18 branch tip used by BeagleBoard's 6.18.x-k3 series.
	major_minor=$(printf '%s\n' "$HEADERS_VERSION" | cut -d. -f1-2)
	patchlevel=$(printf '%s\n' "$HEADERS_VERSION" | cut -d. -f3 | cut -d- -f1)
	for tag in "v${major_minor}.${patchlevel}" "v${major_minor}"; do
		if curl -fsSL \
			"https://raw.githubusercontent.com/gregkh/linux/$tag/drivers/gpu/drm/drm_mipi_dbi.c" \
			-o "$module_dir/drm_mipi_dbi.c" &&
			curl -fsSL \
				"https://raw.githubusercontent.com/gregkh/linux/$tag/drivers/gpu/drm/tiny/ili9341.c" \
				-o "$module_dir/ili9341.c"; then
			log "Fetched DRM sources from gregkh/linux $tag"
			return
		fi
	done
	die "unable to download ili9341 / drm_mipi_dbi sources"
}

build_overlay() {
	log "Building device-tree overlay"
	mkdir -p "$DIST_DIR/dtbo"
	dtc -@ -I dts -O dtb \
		-o "$DIST_DIR/dtbo/k3-am6232-pocketbeagle2-gamepup-a4.dtbo" \
		"$ROOT_DIR/k3-am6232-pocketbeagle2-gamepup-a4.dts"
}

build_userspace() {
	log "Cross-compiling userspace apps ($CROSS_GCC)"
	mkdir -p "$DIST_DIR/bin" "$DIST_DIR/include"
	include_dir=$WORK_DIR/include
	mkdir -p "$include_dir"
	if [ ! -f "$include_dir/libretro.h" ]; then
		curl -fsSL \
			https://raw.githubusercontent.com/libretro/RetroArch/master/libretro-common/include/libretro.h \
			-o "$include_dir/libretro.h"
	fi
	install -m 0644 "$include_dir/libretro.h" "$DIST_DIR/include/libretro.h"

	make -C "$ROOT_DIR/emulator" clean
	make -C "$ROOT_DIR/emulator" -j"$BUILD_JOBS" \
		CC="$CROSS_GCC" \
		LIBRETRO_INCLUDE="$include_dir" \
		KHRONOS_INCLUDE="$ROOT_DIR/emulator/khronos" \
		CFLAGS="-O2 -pipe -std=c11 -Wall -Wextra -Wpedantic -I$include_dir -I$ROOT_DIR/emulator/khronos" \
		LDFLAGS="-L/usr/lib/aarch64-linux-gnu"
	install -m 0755 \
		"$ROOT_DIR/emulator/gamepup-retro" \
		"$ROOT_DIR/emulator/gamepup-oled-status" \
		"$ROOT_DIR/emulator/gamepup-gpu-bench" \
		"$DIST_DIR/bin/"
	make -C "$ROOT_DIR/emulator" clean
}

fetch_git_commit() {
	repository=$1
	commit=$2
	destination=$3

	mkdir -p "$destination"
	git -C "$destination" init -q
	git -C "$destination" remote add origin "$repository"
	git -C "$destination" fetch -q --depth 1 origin "$commit"
	git -C "$destination" checkout -q --detach FETCH_HEAD
}

build_cores() {
	log "Cross-compiling libretro cores"
	mkdir -p "$DIST_DIR/libretro"
	cores_dir=$WORK_DIR/cores
	rm -rf "$cores_dir"
	mkdir -p "$cores_dir"

	fetch_git_commit https://github.com/libretro/nestopia.git \
		"$NESTOPIA_COMMIT" "$cores_dir/nestopia"
	fetch_git_commit https://github.com/libretro/gambatte-libretro.git \
		"$GAMBATTE_COMMIT" "$cores_dir/gambatte"
	fetch_git_commit https://github.com/libretro/libretro-prboom.git \
		"$PRBOOM_COMMIT" "$cores_dir/prboom"

	make -C "$cores_dir/nestopia/libretro" -j"$BUILD_JOBS" \
		platform=unix CC="$CROSS_CXX" CXX="$CROSS_CXX"
	make -C "$cores_dir/gambatte" -f Makefile.libretro -j"$BUILD_JOBS" \
		platform=unix CC="$CROSS_GCC" CXX="$CROSS_CXX"
	make -C "$cores_dir/prboom" -j"$BUILD_JOBS" \
		CC="$CROSS_GCC" CXX="$CROSS_CXX"

	install -m 0755 "$cores_dir/nestopia/libretro/nestopia_libretro.so" \
		"$DIST_DIR/libretro/"
	install -m 0755 "$cores_dir/gambatte/gambatte_libretro.so" \
		"$DIST_DIR/libretro/"
	install -m 0755 "$cores_dir/prboom/prboom_libretro.so" \
		"$DIST_DIR/libretro/"
	install -m 0644 \
		"$cores_dir/nestopia/libretro/libretro-common/include/libretro.h" \
		"$DIST_DIR/include/libretro.h"
}

build_n64() {
	log "Cross-compiling Nintendo 64 core"
	mkdir -p "$DIST_DIR/libretro"
	n64_dir=$WORK_DIR/n64
	rm -rf "$n64_dir"
	fetch_git_commit https://github.com/libretro/mupen64plus-libretro-nx.git \
		"$N64_COMMIT" "$n64_dir"
	make -C "$n64_dir" -j"$BUILD_JOBS" \
		platform=arm64_cortex_a53_gles3 \
		CC="$CROSS_GCC" CXX="$CROSS_CXX"
	install -m 0755 "$n64_dir/mupen64plus_next_libretro.so" \
		"$DIST_DIR/libretro/"
}

build_modules() {
	log "Cross-compiling DRM modules"
	mkdir -p "$DIST_DIR/modules"
	fetch_headers
	log "Using headers tree for $HEADERS_VERSION"
	module_dir=$WORK_DIR/modules
	rm -rf "$module_dir"
	mkdir -p "$module_dir"
	cp "$ROOT_DIR/Makefile" "$module_dir/Makefile"
	fetch_driver_sources "$module_dir"

	case "$HOST_ARCH" in
	aarch64|arm64) ;;
	*)
		wrap_aarch64_host_tools "$KDIR"
		;;
	esac

	make -C "$KDIR" M="$module_dir" \
		ARCH="$TARGET_ARCH" \
		CROSS_COMPILE="${CROSS_TRIPLE}-" \
		CC="$CROSS_GCC" \
		modules
	install -m 0644 "$module_dir/drm_mipi_dbi.ko" "$module_dir/ili9341.ko" \
		"$DIST_DIR/modules/"
	printf '%s\n' "$HEADERS_VERSION" >"$DIST_DIR/modules/KERNEL_VERSION.txt"
}

write_manifest() {
	manifest=$DIST_DIR/MANIFEST.txt
	{
		echo "GamePup cross-build artifacts"
		echo "Target: $TARGET_IMAGE_NAME"
		echo "Image:  $TARGET_IMAGE_FILE"
		echo "Built:  $(date -u +%Y-%m-%dT%H:%M:%SZ)"
		echo "Host:   $(uname -a)"
		echo "CC:     $CROSS_GCC ($($CROSS_GCC --version | head -1))"
		if [ "$SKIP_MODULES" != 1 ]; then
			echo "Kernel: ${HEADERS_VERSION:-unknown}"
		fi
		echo
		echo "Artifacts:"
		find "$DIST_DIR" -type f ! -name MANIFEST.txt | sort | while read -r path; do
			rel=${path#"$DIST_DIR"/}
			sum=$(sha256sum "$path" | awk '{print $1}')
			printf '  %s  %s\n' "$sum" "$rel"
		done
	} >"$manifest"
	log "Wrote $manifest"
}

copy_scripts() {
	mkdir -p "$DIST_DIR/share/bezels" "$DIST_DIR/share/gifs"
	install -m 0644 "$ROOT_DIR/emulator/bezels/"*-system.rgb \
		"$DIST_DIR/share/bezels/" 2>/dev/null || true
	install -m 0644 "$ROOT_DIR/emulator/gifs/"*.gif \
		"$DIST_DIR/share/gifs/" 2>/dev/null || true
	install -m 0755 "$ROOT_DIR/emulator/gamepup-menu" \
		"$ROOT_DIR/emulator/gamepup-hardware-test" \
		"$ROOT_DIR/emulator/gamepup-voice-memo" \
		"$ROOT_DIR/emulator/gamepup-music-player" \
		"$DIST_DIR/bin/"
	install -d -m 0755 "$DIST_DIR/etc/modules-load.d"
	install -m 0644 "$ROOT_DIR/emulator/gamepup-alsa.conf" \
		"$DIST_DIR/etc/modules-load.d/gamepup-alsa.conf"
}

main() {
	require_cmd curl
	require_cmd git
	require_cmd make
	require_cmd dtc
	require_cmd dpkg-deb
	require_cmd file
	require_cmd sha256sum
	pick_cross_gcc

	rm -rf "$DIST_DIR"
	mkdir -p "$DIST_DIR" "$WORK_DIR"

	log "Target image: $TARGET_IMAGE_NAME"
	build_overlay
	build_userspace
	copy_scripts
	if [ "$SKIP_CORES" != 1 ]; then
		build_cores
	fi
	if [ "$SKIP_N64" != 1 ] && [ "$BUILD_N64" = 1 ]; then
		build_n64
	fi
	if [ "$SKIP_MODULES" != 1 ]; then
		require_cmd qemu-aarch64-static
		build_modules
	fi
	write_manifest
	log "Done. Artifacts in $DIST_DIR"
}

main "$@"
