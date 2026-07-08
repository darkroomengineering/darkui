#!/bin/bash

killshow()
{
	SHOW_ID=`pidof show.elf`
	kill $SHOW_ID
	wait $SHOW_ID 2>/dev/null
}

DIR="$(dirname "$0")"
cd "$DIR"

show.elf "$DIR/res/preparing.png" 60 &

# PATH doesn't like spaces in paths
cp -rf bin /tmp/
PATH=/tmp/bin:$PATH

DEV_PATH=/dev/mmcblk0

killshow

# a backup and its accompanying offset/size metadata (written by apply.sh
# at the time it made the backup) are both required to safely restore;
# without the metadata we don't know where on the device the backup
# region belongs, so we refuse to guess rather than risk writing to the
# wrong offset. this is also the safe no-op path for a device that was
# never patched.
if [ ! -s "$DIR/PanelFix-backup.dtb" ] || [ ! -s "$DIR/PanelFix-backup.meta" ]; then
	show.elf "$DIR/res/mismatch.png" 2
	echo "no panel fix backup found, nothing to restore"
	exit 0
fi

# read DTB_OFFSET / DTB_SIZE recorded by apply.sh alongside the backup,
# rather than re-deriving them here (keeps a single source of truth for
# the offset-detection logic in apply.sh)
source "$DIR/PanelFix-backup.meta"

if [ -z "$DTB_OFFSET" ] || [ -z "$DTB_SIZE" ]; then
	show.elf "$DIR/res/fail.png" 2
	echo "backup metadata is incomplete, aborting restore"
	exit 1
fi

show.elf "$DIR/res/applying.png" 60 &

dd if="$DIR/PanelFix-backup.dtb" of=$DEV_PATH bs=1 seek=$DTB_OFFSET conv=notrunc 2>/dev/null # restore
sync

killshow
echo "restored original dtb from backup"

show.elf "$DIR/res/rebooting.png" 2
reboot
