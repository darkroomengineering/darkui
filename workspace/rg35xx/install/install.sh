#!/system/bin/sh

# NOTE: this file is not chrooted so it's using stock's everything!
# {

TF1_PATH=/mnt/mmc
TF2_PATH=/mnt/sdcard # TF1 should be linked to this path if TF2 is missing or doesn't contain our system folder
SYSTEM_PATH=${TF2_PATH}/.system/rg35xx
FLAG_PATH=/misc/.darkuinstalled

echo "installing/updating"

if [ ! -f $FLAG_PATH ]; then
	echo "backing up"
	BAK_PATH=$TF1_PATH/bak
	mkdir -p $BAK_PATH
	cp /misc/boot_logo.bmp.gz $BAK_PATH
fi

was_updated() {
	# intiial releases didn't install this properly :sob:
	if [ ! -f /misc/charging.png ]; then
		return 0
	fi
	
	for FILE in /misc/* /misc/*/*; do
		A_PATH=$FILE
		A_NAME=$(busybox basename "$A_PATH")
		B_PATH=$SYSTEM_PATH/dat/$A_NAME
		
		if [ "$A_NAME" = "boot_logo.bmp.gz" ]; then
			# we don't care if the user has changed their boot logo
			continue
		fi
		
		if [ "$A_NAME" = "charging.png" ]; then
			# we don't care if the user has changed their charging image
			continue
		fi
		
		if [ ! -f "$B_PATH" ]; then
			continue
		fi
	
		A_SUM=$(busybox md5sum $A_PATH | busybox cut -d ' ' -f 1)
		B_SUM=$(busybox md5sum $B_PATH | busybox cut -d ' ' -f 1)
	
		if [ "$A_SUM" != "$B_SUM" ]; then
			return 0
		fi
	done
	
	return 1
}

if [ ! -f $FLAG_PATH ] || was_updated; then
	echo "updating misc partition"
	mount -o remount,rw /dev/block/actb /misc
	# only mark the install complete once the critical boot files copied
	# successfully; a failed copy leaves $FLAG_PATH unset so the next boot
	# retries instead of rebooting into a half-written /misc
	if cp $SYSTEM_PATH/dat/dmenu.bin /misc \
		&& cp $SYSTEM_PATH/dat/ramdisk.img /misc \
		&& cp $SYSTEM_PATH/dat/boot_logo.bmp.gz /misc; then
		# boot logo (darkUI branding) is refreshed on every install/update above
		# charging graphic, only installed, never updated
		if [ ! -f /misc/charging.png ]; then
			cp $SYSTEM_PATH/dat/charging.png /misc
		fi

		touch $FLAG_PATH
		sync && reboot
	else
		echo "misc copy failed; leaving update pending for retry"
		sync
		# return non-zero so boot.sh keeps MinUI.zip (its `install.sh && rm`)
		# and the update is retried on the next boot instead of being lost
		exit 1
	fi
fi

# } &> /mnt/sdcard/install.txt
