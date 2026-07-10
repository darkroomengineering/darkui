#!/bin/bash
# Push a darkUI release onto an RG35XX (original, not Plus/SP) over USB, using
# the stock adbd that runs at every boot on that platform. Lets you update the
# OS without pulling the SD card out.
#
# Usage: scripts/rg35xx-usb-update.sh [release-dir]
#   release-dir defaults to ./build/BASE

set -euo pipefail

RELEASE_DIR="${1:-./build/BASE}"
MINUI_ZIP="$RELEASE_DIR/MinUI.zip"
DMENU_BIN="$RELEASE_DIR/rg35xx/dmenu.bin"

if ! command -v adb >/dev/null 2>&1; then
	echo "error: adb not found on PATH." >&2
	echo "hint: brew install android-platform-tools" >&2
	exit 1
fi

if [ ! -f "$MINUI_ZIP" ]; then
	echo "error: $MINUI_ZIP not found." >&2
	exit 1
fi

if [ ! -f "$DMENU_BIN" ]; then
	echo "error: $DMENU_BIN not found." >&2
	exit 1
fi

echo "Waiting for the RG35XX over USB..."
echo "(make sure the console is sitting at the launcher, NOT in USB mode --"
echo " /mnt/mmc is unmounted while the console is in USB mode)"
adb wait-for-device

echo "Checking that ROMS is mounted at /mnt/mmc..."
if ! adb shell 'busybox mountpoint -q /mnt/mmc 2>/dev/null || ls /mnt/mmc/.system' >/dev/null 2>&1; then
	echo "error: /mnt/mmc doesn't look mounted on the console." >&2
	echo "Back out of USB mode (if the console is in it) and return to the launcher, then re-run this script." >&2
	exit 1
fi

echo "Pushing $MINUI_ZIP -> /mnt/mmc/MinUI.zip"
adb push "$MINUI_ZIP" /mnt/mmc/MinUI.zip

echo "Pushing $DMENU_BIN -> /misc/dmenu.bin"
# /misc mounts read-only in the stock environment
adb shell 'mount -o remount,rw /misc'
adb push "$DMENU_BIN" /misc/dmenu.bin
adb shell 'sync; mount -o remount,ro /misc' || true # busy is fine, we reboot next

echo "Syncing and rebooting..."
adb shell sync
adb reboot

echo
echo "Done. The console will show the \"Updating darkUI...\" splash on boot"
echo "and land in the new build."
