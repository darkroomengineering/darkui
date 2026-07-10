#!/bin/sh
# Shared MTP-over-USB control script (rg35xxplus).
#
# Wraps the uMTP-Responder configfs USB gadget so both the boot-time
# autostart hook (MinUI.pak/launch.sh) and the "USB Storage.pak" manual
# toggle share one implementation. The gadget setup/teardown sequence below
# is copied verbatim from the working USB Storage.pak/launch.sh -- do not
# reorder it, the enumeration depends on this exact order.

SYSTEM_PATH="${SYSTEM_PATH:-/mnt/sdcard/.system/rg35xxplus}"
BIN_DIR="$SYSTEM_PATH/bin"
UMTPRD="$BIN_DIR/umtprd"
UMTPRD_CONF="$BIN_DIR/umtprd.conf"

GADGET_DIR="/sys/kernel/config/usb_gadget/darkui"
FFS_MOUNT="/dev/ffs-mtp"
UDC_NAME="5100000.udc-controller"

LOG="/tmp/mtp.txt"

daemon_pid() {
	pidof umtprd
}

udc_bound() {
	[ -s "$GADGET_DIR/UDC" ]
}

is_up() {
	[ -n "$(daemon_pid)" ] && udc_bound
}

teardown() {
	# Detach from the USB bus first so the host drops the device cleanly.
	echo "" > "$GADGET_DIR/UDC" 2>/dev/null

	pids="$(daemon_pid)"
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
}

bring_up() {
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
	setsid "$UMTPRD" -conf "$UMTPRD_CONF" &

	sleep 1

	echo "$UDC_NAME" > "$GADGET_DIR/UDC"
}

cmd_start() {
	if [ -d "$GADGET_DIR" ]; then
		if [ -n "$(daemon_pid)" ]; then
			# already up: nothing to do
			return 0
		fi
		# gadget dir left behind but the daemon died: clear it before retrying
		teardown
	fi
	bring_up
}

cmd_stop() {
	teardown
}

cmd_status() {
	if is_up; then
		echo "on"
		return 0
	fi
	echo "off"
	return 1
}

case "$1" in
	start)
		{ cmd_start; } >>"$LOG" 2>&1
		;;
	stop)
		{ cmd_stop; } >>"$LOG" 2>&1
		;;
	status)
		cmd_status
		exit $?
		;;
	*)
		echo "usage: $0 {start|stop|status}" >&2
		exit 1
		;;
esac
