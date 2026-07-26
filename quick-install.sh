#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Download the current release source and run the complete on-device installer.

set -eu

REPOSITORY=${GAMEPUP_REPOSITORY:-Grippy98/PocketBeagle2-GamePup-Demo}
REVISION=${GAMEPUP_REVISION:-main}
ARCHIVE_URL="https://github.com/$REPOSITORY/archive/refs/heads/$REVISION.tar.gz"
DOWNLOAD_DIR=$(mktemp -d /tmp/gamepup-quick-install.XXXXXX)
ARCHIVE_PATH=$DOWNLOAD_DIR/source.tar.gz
SOURCE_DIR=$DOWNLOAD_DIR/source

cleanup()
{
	case "$DOWNLOAD_DIR" in
	/tmp/gamepup-quick-install.*) find "$DOWNLOAD_DIR" -depth -delete ;;
	esac
}
trap cleanup EXIT HUP INT TERM

if [ "$(id -u)" -ne 0 ]; then
	echo "Run with root privileges, for example: curl ... | sudo sh" >&2
	exit 1
fi

for command in curl tar; do
	if ! command -v "$command" >/dev/null 2>&1; then
		echo "$command is required by the quick installer." >&2
		exit 1
	fi
done

echo "Downloading $REPOSITORY at $REVISION..."
mkdir "$SOURCE_DIR"
curl --fail --location --proto '=https' --tlsv1.2 \
	--output "$ARCHIVE_PATH" "$ARCHIVE_URL"
tar -xzf "$ARCHIVE_PATH" --strip-components=1 -C "$SOURCE_DIR"

"$SOURCE_DIR/install-all.sh"
