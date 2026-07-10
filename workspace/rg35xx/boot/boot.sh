#!/system/bin/sh

# NOTE: unlike workspace/rg35xxplus/boot/boot.sh, this script has no
# `mountpoint -q` remount guards and no `>> $TF1_PATH/log.txt` logging.
# That's intentional, not an oversight: this script runs under the stock
# rg35xx (Android-derived) boot environment, which appears to ship only a
# minimal busybox -- every non-trivial command below (grep, cut, tail,
# uudecode, unzip, fbset, losetup, chroot) is invoked as `busybox <cmd>`
# rather than as a standalone binary, and no `mountpoint` (busybox applet or
# otherwise) is used anywhere else in this tree. There is no evidence a
# `mountpoint` binary exists in this boot environment, so backporting the
# Plus variant's guard would risk silently failing (or hanging) partway
# through boot with no fallback. Do not backport without first confirming
# `mountpoint` (or `busybox mountpoint`) is actually present on-device.

/usbdbg.sh device

TF1_PATH=/mnt/mmc # ROMS partition
TF2_PATH=/mnt/sdcard
SDCARD_PATH=$TF1_PATH
SYSTEM_DIR=/.system
SYSTEM_FRAG=$SYSTEM_DIR/rg35xx
UPDATE_FRAG=/MinUI.zip
SYSTEM_PATH=${SDCARD_PATH}${SYSTEM_FRAG}
UPDATE_PATH=${SDCARD_PATH}${UPDATE_FRAG}

mkdir /mnt/sdcard
if [ -e /dev/block/mmcblk1p1 ]; then
	SDCARD_DEVICE=/dev/block/mmcblk1p1
else
	SDCARD_DEVICE=/dev/block/mmcblk1
fi
mount -t vfat -o rw,utf8,noatime $SDCARD_DEVICE /mnt/sdcard
if [ $? -ne 0 ]; then
	mount -t exfat -o rw,utf8,noatime $SDCARD_DEVICE /mnt/sdcard
	if [ $? -ne 0 ]; then
		rm -rf $TF2_PATH
		ln -s $TF1_PATH $TF2_PATH
	fi
fi

if [ -d ${TF1_PATH}${SYSTEM_FRAG} ] || [ -f ${TF1_PATH}${UPDATE_FRAG} ]; then
	if [ ! -L $TF2_PATH ]; then
		# .system found on TF1 but TF2 is present
		# so unmount and symlink to TF1 path
		umount $TF2_PATH
		rm -rf $TF2_PATH
		ln -s $TF1_PATH $TF2_PATH
	fi
fi

SDCARD_PATH=$TF2_PATH
SYSTEM_PATH=${SDCARD_PATH}${SYSTEM_FRAG}
UPDATE_PATH=${SDCARD_PATH}${UPDATE_FRAG}

# USB storage mode: a Tools pak touches this flag and reboots. Export the
# card(s) to the PC before anything mounts rootfs.ext2 (which lives on the
# ROMS partition -- block-level export while mounted would corrupt it).
USB_FLAG=$TF1_PATH/.usbmode
if [ -f $USB_FLAG ]; then
	rm -f $USB_FLAG
	CUT=$((`busybox grep -n '^BINARY' $0 | busybox cut -d ':' -f 1 | busybox tail -1` + 1))
	busybox tail -n +$CUT "$0" | busybox uudecode -o /tmp/data
	busybox unzip -o /tmp/data -d /tmp
	busybox fbset -g 640 480 640 480 16
	dd if=/tmp/usbmode of=/dev/fb0
	sync

	USB_OK=1
	if [ ! -L $TF2_PATH ]; then
		umount $TF2_PATH || USB_OK=0
	fi
	umount $TF1_PATH || USB_OK=0

	if [ $USB_OK = 1 ]; then
		/usbmond.sh ADD_LUN LUN0 /dev/block/mmcblk0p4
		if [ -e /dev/block/mmcblk1 ] && [ -n "$SDCARD_DEVICE" ]; then
			/usbmond.sh ADD_LUN LUN1 $SDCARD_DEVICE
		fi

		STATE=/sys/class/android_usb/android0/state

		# give the host up to 15s to enumerate before watching for unplug,
		# otherwise a not-yet-bound cable reads as disconnected and we bail
		WAIT=15
		while [ $WAIT -gt 0 ]; do
			if [ -f $STATE ] && [ "`busybox cat $STATE`" = "CONFIGURED" ]; then
				break
			fi
			sleep 1
			WAIT=$((WAIT - 1))
		done

		# stay exported while the cable is attached; leave when unplugged
		IDLE=0
		while [ $IDLE -lt 3 ]; do
			sleep 1
			if [ -f $STATE ] && [ "`busybox cat $STATE`" = "CONFIGURED" ]; then
				IDLE=0
			else
				IDLE=$((IDLE + 1))
			fi
		done

		/usbmond.sh REMOVE_LUN LUN0
		/usbmond.sh REMOVE_LUN LUN1
	fi

	sync
	reboot
fi

# is there an update available?
if [ -f $UPDATE_PATH ]; then
	FLAG_PATH=/misc/.darkuinstalled
	if [ ! -f $FLAG_PATH ]; then
		ACTION=installing
	else
		ACTION=updating
	fi

	# extract the zip file appended to the end of this script to tmp
	# and display one of the two images it contains 
	CUT=$((`busybox grep -n '^BINARY' $0 | busybox cut -d ':' -f 1 | busybox tail -1` + 1))
	busybox tail -n +$CUT "$0" | busybox uudecode -o /tmp/data
	busybox unzip -o /tmp/data -d /tmp
	busybox fbset -g 640 480 640 480 16
	dd if=/tmp/$ACTION of=/dev/fb0
	sync
	
	# only finish the install if the extraction fully succeeded; a failed or
	# partial unzip leaves MinUI.zip in place so the next boot retries cleanly
	# instead of running install.sh against a half-written system. The zip is
	# removed only after install.sh returns success (it reboots mid-install on a
	# real install, so the zip is cleared on the following idempotent pass).
	if busybox unzip -o $UPDATE_PATH -d $SDCARD_PATH; then
		$SYSTEM_PATH/bin/install.sh && rm -f $UPDATE_PATH # &> $SDCARD_PATH/install.txt
	fi
fi

ROOTFS_IMAGE=$SYSTEM_PATH/rootfs.ext2
if [ ! -f $ROOTFS_IMAGE ]; then
	# fallback to stock demenu.bin, based on dmenu_ln
	ACT="/tmp/.next"
	CMD="/mnt/vendor/bin/dmenu.bin"
	touch "$ACT"
	while [ -f $CMD ]; do
		if $CMD; then
			if [ -f "$ACT" ]; then
				if  ! sh $ACT; then
					echo
				fi
				rm -f "$ACT"
			fi
		fi
	done
	sync && reboot -p
fi

ROOTFS_MOUNTPOINT=/cfw
LOOPDEVICE=/dev/block/loop7
mkdir $ROOTFS_MOUNTPOINT
busybox losetup $LOOPDEVICE $ROOTFS_IMAGE
mount -r -w -o loop -t ext4 $LOOPDEVICE $ROOTFS_MOUNTPOINT
rm -rf $ROOTFS_MOUNTPOINT/tmp/*
mkdir $ROOTFS_MOUNTPOINT/mnt/mmc
mkdir $ROOTFS_MOUNTPOINT/mnt/sdcard
for f in dev dev/pts proc sys mnt/mmc mnt/sdcard # tmp doesn't work for some reason?
do
	mount -o bind /$f $ROOTFS_MOUNTPOINT/$f
done

export PATH=/usr/sbin:/usr/bin:/sbin:/bin:$PATH
export LD_LIBRARY_PATH=/usr/lib/:/lib/
export HOME=$SDCARD_PATH
busybox chroot $ROOTFS_MOUNTPOINT $SYSTEM_PATH/paks/MinUI.pak/launch.sh

umount $ROOTFS_MOUNTPOINT
busybox losetup --detach $LOOPDEVICE
sync && reboot -p

exit 0
