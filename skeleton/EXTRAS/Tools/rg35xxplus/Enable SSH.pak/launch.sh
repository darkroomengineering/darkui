#!/bin/bash
DIR="$(dirname "$0")"
cd "$DIR"

{

show.elf "$DIR/ssh.png" 300 &

# Bring Wi-Fi up if it isn't (uses .userdata/wifi.txt, same as boot).
if [ "$(cat /sys/class/net/wlan0/operstate 2>/dev/null)" != "up" ]; then
	"$SYSTEM_PATH/bin/wifi.sh"
	i=0
	while [ "$i" -lt 40 ]; do
		[ "$(cat /sys/class/net/wlan0/operstate 2>/dev/null)" = "up" ] && break
		sleep 0.5
		i=$((i+1))
	done
fi

# Still no Wi-Fi -> show the hint and bail.
if [ "$(cat /sys/class/net/wlan0/operstate 2>/dev/null)" != "up" ]; then
	killall -9 show.elf
	show.elf "$DIR/wifi.png" 8
	exit 0
fi

# Install the SSH server only if it's missing (needs the network we just brought up).
if ! command -v sshd >/dev/null 2>&1; then
	locale-gen "en_US.UTF-8"
	echo "LANG=en_US.UTF-8" > /etc/default/locale
	apt -y update && apt -y install --reinstall openssh-server
fi

# Allow root:root and (re)start the server.
echo "d /run/sshd 0755 root root" > /etc/tmpfiles.d/sshd.conf
grep -q "^PermitRootLogin yes" /etc/ssh/sshd_config || echo "PermitRootLogin yes" >> /etc/ssh/sshd_config
printf "root\nroot" | passwd root
mkdir -p /run/sshd
systemctl enable ssh 2>/dev/null
systemctl restart ssh 2>/dev/null || /usr/sbin/sshd 2>/dev/null

# Report the address so it's easy to connect.
IP=$(ip -4 addr show wlan0 2>/dev/null | grep -o 'inet [0-9.]*' | awk '{print $2}')
echo "ssh root@$IP  (password: root)" > "$DIR/last-ip.txt"

killall -9 show.elf
show.elf "$DIR/ssh.png" 5

} &> ./log.txt
