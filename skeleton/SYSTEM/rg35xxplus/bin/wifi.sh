#!/bin/sh
# Opt-in Wi-Fi bring-up so the (already-installed) SSH server is reachable in darkUI
# without going back to stock firmware. No-op unless the user drops a credentials file
# at .userdata/wifi.txt (line 1 = SSID, line 2 = password; the password stays on the
# card and is never committed to the repo).

CONF="${SDCARD_PATH:-/mnt/sdcard}/.userdata/wifi.txt"
[ -f "$CONF" ] || exit 0

SSID=$(sed -n '1p' "$CONF" | tr -d '\r\n')
PSK=$(sed -n '2p' "$CONF" | tr -d '\r\n')
[ -n "$SSID" ] || exit 0

# already connected? nothing to do
[ "$(cat /sys/class/net/wlan0/operstate 2>/dev/null)" = "up" ] && exit 0

ip link set wlan0 up 2>/dev/null

WPA=/tmp/wifi.conf
if [ -n "$PSK" ]; then
	wpa_passphrase "$SSID" "$PSK" > "$WPA" 2>/dev/null || \
		printf 'network={\n\tssid="%s"\n\tpsk="%s"\n}\n' "$SSID" "$PSK" > "$WPA"
else
	printf 'network={\n\tssid="%s"\n\tkey_mgmt=NONE\n}\n' "$SSID" > "$WPA"
fi

killall -q wpa_supplicant 2>/dev/null
wpa_supplicant -B -i wlan0 -c "$WPA" 2>/dev/null

# wait up to ~15s for association
i=0
while [ "$i" -lt 30 ]; do
	wpa_cli -i wlan0 status 2>/dev/null | grep -q "wpa_state=COMPLETED" && break
	sleep 0.5
	i=$((i+1))
done

# lease an address (background so boot isn't blocked)
( dhclient -nw wlan0 2>/dev/null || udhcpc -b -i wlan0 2>/dev/null ) &
