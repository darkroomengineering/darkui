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

# const
DT_NAME=device
DEV_PATH=/dev/mmcblk0
# BOOT_OFFSET=$((16400 * 1024))
# DTB_OFFSET=$((BOOT_OFFSET+1161216))
DTB_OFFSET=17954816
DTB_MAGIC=`xxd -s $DTB_OFFSET -l4 -ps $DEV_PATH`

if [ "$DTB_MAGIC" != "d00dfeed" ]; then
	echo "bad DTB_MAGIC at $DTB_OFFSET"
	DTB_OFFSET=17971200 # alternate dtb location
	DTB_MAGIC=`xxd -s $DTB_OFFSET -l4 -ps $DEV_PATH`
	
	if [ "$DTB_MAGIC" != "d00dfeed" ]; then
		echo "bad DTB_MAGIC at $DTB_OFFSET"
		killshow
		show.elf "$DIR/res/fail.png" 2
		echo "unable to decompile dtb, aborting"
		exit 1
	fi
fi

# var
SIZE_OFFSET=$((DTB_OFFSET+4))
DTB_SIZE=$((0x`xxd -s $SIZE_OFFSET -l4 -ps $DEV_PATH`))

dd if=$DEV_PATH of=$DT_NAME.dtb bs=1 skip=$DTB_OFFSET count=$DTB_SIZE 2>/dev/null # extract
dtc -I dtb -O dts -o $DT_NAME.dts $DT_NAME.dtb 2>/dev/null # decompile

if [ ! -f $DT_NAME.dts ]; then
	killshow
	show.elf "$DIR/res/fail.png" 2
	echo "unable to decompile dtb, aborting"
	exit 1
fi

CF= # lcd_dclk_freq
HT= # lcd_ht
VT= # lcd_vt
BL= # lcd_backlight

# parse the values out of the decompiled dtb
while read -r LINE; do
    KEY=$(echo "$LINE" | grep -oP '^[\w_]+')
    HEX=$(echo "$LINE" | grep -oP '0x[\dA-Fa-f]+')
	DEC=$(($HEX))

	case $KEY in
	lcd_dclk_freq)
		CF=$DEC
		;;
	lcd_ht)
		HT=$DEC
		;;
	lcd_vt)
		VT=$DEC
		;;
	lcd_backlight)
		BL=$DEC
		;;
	esac
# redirect instead of using a pipe so variables stay in scope
done < <(grep 'lcd_backlight\|lcd_dclk_freq\|lcd_ht\|lcd_vt' "$DT_NAME.dts")

echo "    CF HT  VT  (BL)"
echo "IN  $CF-$HT-$VT ($BL)"

# match to known (incorrect) values
CF_OFFSET=0
HT_OFFSET=0
VT_OFFSET=0
case $CF-$HT-$VT in
24-770-526) # 35xx2024/35xxPLUS
	HT_OFFSET=-2 # 768
	VT_OFFSET=-5 # 521
	;;
24-770-528) # 35xxH
	HT_OFFSET=-2 # 768
	VT_OFFSET=-7 # 521
	;;
24-586-686) # 28xx
	HT_OFFSET=-2 # 584
	VT_OFFSET=-1 # 685
	;;
24-770-525) # 35xxSP
	HT_OFFSET=-2 # 768
	VT_OFFSET=-4 # 521
	;;
24-720-560) # 35xxSP stock V1.1.5 (frz) — 59.52Hz stock, 59.99Hz patched
	HT_OFFSET=-3 # 717
	VT_OFFSET=-2 # 558
	;;
24-770-520) # Ry's 35xxSP
	HT_OFFSET=-2 # 768
	VT_OFFSET= 1 # 521
	;;
24-770-522) # 40xxH
	HT_OFFSET=-2 # 768
	VT_OFFSET=-1 # 521
	;;
25-728-568) # 35xxSP 1.0.6
	# the numbers don't add up but 
	# this is already hitting 60fps
	# so fudge BL to exit early
	echo "known good config"
	BL=51
	;;
36-812-756) # CubeXX
	CF_OFFSET=1 # 37
	;;
26-820-536) # 34xx
	HT_OFFSET=-7 # 813
	VT_OFFSET=-3 # 533
	;;
esac

# update values or bail
BL_OFFSET=$((CF_OFFSET+HT_OFFSET+VT_OFFSET))
if [ $BL_OFFSET -ne 0 ]; then
	CF=$((CF+CF_OFFSET))
	HT=$((HT+HT_OFFSET))
	VT=$((VT+VT_OFFSET))
	BL=$((BL-BL_OFFSET))
else
	killshow
	if [ $BL -ne 50 ]; then
		show.elf "$DIR/res/patched.png" 2
		echo "this panel has (probably) already been patched"
	else
		show.elf "$DIR/res/unknown.png" 2
		echo "unrecognized panel configuration, aborting"
	fi
	exit 0
fi

echo "OUT $CF-$HT-$VT ($BL)"

# dupe and inject updated values
MOD_PATH=$DT_NAME-mod.dts
cp $DT_NAME.dts $MOD_PATH
sed -i "s/\(lcd_dclk_freq = <\)0x[0-9A-Fa-f]\+/\1$CF/" "$MOD_PATH"
sed -i "s/\(lcd_ht = <\)0x[0-9A-Fa-f]\+/\1$HT/" "$MOD_PATH"
sed -i "s/\(lcd_vt = <\)0x[0-9A-Fa-f]\+/\1$VT/" "$MOD_PATH"
sed -i "s/\(lcd_backlight = <\)0x[0-9A-Fa-f]\+/\1$BL/" "$MOD_PATH"

dtc -I dts -O dtb -o $DT_NAME-mod.dtb $DT_NAME-mod.dts 2>/dev/null # recompile
dd if=$DT_NAME.dtb of=$DT_NAME-mod.dtb bs=1 skip=4 seek=4 count=4 conv=notrunc 2>/dev/null # inject original size
fallocate -l $DTB_SIZE $DT_NAME-mod.dtb # zero fill empty space

killshow
show.elf "$DIR/res/verifying.png" 60 &

# validate checksum
A_PATH=$DT_NAME.dtb
B_PATH=$DT_NAME-mod.dtb

declare -a BYTES
BYTES=(`xxd -p -c 1 $A_PATH | tr '\n' ' '`)
SUM=0
for (( i=0; i<${#BYTES[@]}; i++)); do
	SUM=$((SUM+0x${BYTES[i]}))
done
A_SUM=$SUM

declare -a BYTES
BYTES=(`xxd -p -c 1 $B_PATH | tr '\n' ' '`)
SUM=0
for (( i=0; i<${#BYTES[@]}; i++)); do
	SUM=$((SUM+0x${BYTES[i]}))
done
B_SUM=$SUM

if [ $A_SUM != $B_SUM ]; then
	killshow
	show.elf "$DIR/res/mismatch.png" 2
	echo "A $A_SUM"
	echo "B $B_SUM"
	echo "mismatched checksum, aborting"
	exit 1
fi

killshow
show.elf "$DIR/res/applying.png" 60 &

# best-effort backup of the original on-device DTB region to durable SD
# storage before overwriting it; same offset/size as the write below, and
# a failed backup must never abort the patch
dd if=$DEV_PATH bs=1 skip=$DTB_OFFSET count=$DTB_SIZE of="$DIR/PanelFix-backup.dtb" 2>/dev/null || true

# record the offset/size alongside the backup so restore.sh doesn't need to
# re-derive them (avoids duplicating the device-detection logic above);
# a failed write here must never abort the patch
{ echo "DTB_OFFSET=$DTB_OFFSET"; echo "DTB_SIZE=$DTB_SIZE"; } > "$DIR/PanelFix-backup.meta" 2>/dev/null || true

dd if=$DT_NAME-mod.dtb of=$DEV_PATH bs=1 seek=$DTB_OFFSET conv=notrunc 2>/dev/null # inject
sync

# post-write readback verification: confirm the bytes we intended to write
# actually landed on the device before declaring success. reuses the same
# additive-checksum mechanism as the pre-write validation above for
# consistency. B_SUM (checksum of $DT_NAME-mod.dtb) is still valid here
# since that file has not been touched since it was computed.
dd if=$DEV_PATH bs=1 skip=$DTB_OFFSET count=$DTB_SIZE of="$DT_NAME-readback.dtb" 2>/dev/null

declare -a BYTES
BYTES=(`xxd -p -c 1 $DT_NAME-readback.dtb | tr '\n' ' '`)
SUM=0
for (( i=0; i<${#BYTES[@]}; i++)); do
	SUM=$((SUM+0x${BYTES[i]}))
done
READBACK_SUM=$SUM

if [ "$READBACK_SUM" != "$B_SUM" ]; then
	killshow
	show.elf "$DIR/res/fail.png" 2
	echo "READBACK $READBACK_SUM"
	echo "EXPECTED $B_SUM"
	echo "post-write readback verification failed"

	# safety net: restore the original dtb from the pre-write backup,
	# but only if we actually have one to restore from
	if [ -s "$DIR/PanelFix-backup.dtb" ]; then
		dd if="$DIR/PanelFix-backup.dtb" of=$DEV_PATH bs=1 seek=$DTB_OFFSET conv=notrunc 2>/dev/null
		sync
		echo "restored original dtb from backup"
	fi

	exit 1
fi

echo "applied fix successfully!"

show.elf "$DIR/res/rebooting.png" 2
reboot
