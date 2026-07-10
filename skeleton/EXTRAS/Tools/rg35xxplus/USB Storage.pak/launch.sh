#!/bin/sh
DIR="$(dirname "$0")"
cd "$DIR"

SYSTEM_PATH="${SYSTEM_PATH:-/mnt/sdcard/.system/rg35xxplus}"

{
	if "$SYSTEM_PATH/bin/mtp.sh" status >/dev/null 2>&1; then
		"$SYSTEM_PATH/bin/mtp.sh" stop
		show.elf "$DIR/mtp-off.png" 5
	else
		"$SYSTEM_PATH/bin/mtp.sh" start
		show.elf "$DIR/mtp-on.png" 5
	fi
} > ./log.txt 2>&1
