#!/bin/bash
DIR="$(dirname "$0")"
cd "$DIR"

GADGET_DIR="/sys/kernel/config/usb_gadget/darkui"
FFS_MOUNT="/dev/ffs-mtp"
UDC_NAME="5100000.udc-controller"

{

if [ -d "$GADGET_DIR" ]; then
	# --- MTP is currently on: turn it off ---

	# Detach from the USB bus first so the host drops the device cleanly.
	echo "" > "$GADGET_DIR/UDC" 2>/dev/null

	pids="$(pidof umtprd)"
	if [ -n "$pids" ]; then
		kill $pids 2>/dev/null
		sleep 1
		kill -9 $pids 2>/dev/null
	fi

	umount "$FFS_MOUNT" 2>/dev/null
	rmdir "$FFS_MOUNT" 2>/dev/null

	rm -f "$GADGET_DIR/configs/c.1/ffs.mtp"
	rmdir "$GADGET_DIR/functions/ffs.mtp" 2>/dev/null
	rmdir "$GADGET_DIR/configs/c.1/strings/0x409" 2>/dev/null
	rmdir "$GADGET_DIR/configs/c.1" 2>/dev/null
	rmdir "$GADGET_DIR/strings/0x409" 2>/dev/null
	rmdir "$GADGET_DIR" 2>/dev/null

	show.elf "$DIR/mtp-off.png" 5

else
	# --- MTP is currently off: turn it on ---

	modprobe libcomposite 2>/dev/null

	mkdir -p "$GADGET_DIR"
	echo 0x18d1 > "$GADGET_DIR/idVendor"
	echo 0x4ee1 > "$GADGET_DIR/idProduct"
	echo 0x0100 > "$GADGET_DIR/bcdDevice"
	echo 0x0200 > "$GADGET_DIR/bcdUSB"

	mkdir -p "$GADGET_DIR/strings/0x409"
	echo "00000000" > "$GADGET_DIR/strings/0x409/serialnumber"
	echo "Darkroom" > "$GADGET_DIR/strings/0x409/manufacturer"
	echo "darkUI RG35XXSP" > "$GADGET_DIR/strings/0x409/product"

	mkdir -p "$GADGET_DIR/configs/c.1/strings/0x409"
	echo "MTP" > "$GADGET_DIR/configs/c.1/strings/0x409/configuration"
	echo 120 > "$GADGET_DIR/configs/c.1/MaxPower"

	mkdir -p "$GADGET_DIR/functions/ffs.mtp"
	ln -s "$GADGET_DIR/functions/ffs.mtp" "$GADGET_DIR/configs/c.1/ffs.mtp"

	mkdir -p "$FFS_MOUNT"
	mount -t functionfs mtp "$FFS_MOUNT"

	# umtprd must open the ffs ep0/ep1/ep2/ep3 endpoints before the UDC is
	# bound, or the kernel has nothing to hand the host during enumeration.
	setsid "$DIR/umtprd" -conf "$DIR/umtprd.conf" &

	sleep 1

	echo "$UDC_NAME" > "$GADGET_DIR/UDC"

	show.elf "$DIR/mtp-on.png" 5
fi

} &> ./log.txt
