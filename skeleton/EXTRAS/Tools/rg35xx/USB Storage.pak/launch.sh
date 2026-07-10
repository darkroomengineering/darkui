#!/bin/sh

cd $(dirname "$0")

# Flag the ROMS partition (bind-mounted here inside the chroot) so the next
# boot -- before rootfs.ext2 is mounted -- exports it (and TF2, if present)
# to a connected PC over USB mass storage until the cable is unplugged.
touch /mnt/mmc/.usbmode
sync
reboot -f
