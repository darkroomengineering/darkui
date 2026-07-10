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
# SSID/PSK are attacker-influenced (SSID comes from a nearby AP's beacon, PSK is
# whatever was persisted for it) -- escape backslash and double-quote so neither
# can break out of the quoted ssid="..."/psk="..." values written below. CR/LF
# were already stripped when $CONF was read above.
esc_ssid=$(printf '%s' "$SSID" | sed 's/\\/\\\\/g; s/"/\\"/g')
esc_psk=$(printf '%s' "$PSK" | sed 's/\\/\\\\/g; s/"/\\"/g')
# ctrl_interface lets wpa_cli (and the Wi-Fi tool) manage the connection;
# update_config=1 lets it persist networks back to the config.
{
	echo "ctrl_interface=/var/run/wpa_supplicant"
	echo "update_config=1"
	if [ -n "$PSK" ]; then
		wpa_out=$(wpa_passphrase "$SSID" "$PSK" 2>/dev/null)
		if [ -n "$wpa_out" ]; then
			# never trust wpa_passphrase's echoed ssid="..." line (it's the raw,
			# unescaped SSID) -- drop it and inject our own escaped value instead.
			# ESC_SSID is read via ENVIRON, not -v, because awk's -v assignment
			# re-interprets backslash escapes (POSIX "string constant" rules) and
			# would silently strip the \" / \\ escaping sed just applied.
			printf '%s\n' "$wpa_out" | ESC_SSID="$esc_ssid" awk '
				/^[ \t]*ssid=/ { next }
				{ print }
				/^network=\{/ { printf "\tssid=\"%s\"\n", ENVIRON["ESC_SSID"] }
			'
		else
			printf 'network={\n\tssid="%s"\n\tpsk="%s"\n}\n' "$esc_ssid" "$esc_psk"
		fi
	else
		printf 'network={\n\tssid="%s"\n\tkey_mgmt=NONE\n}\n' "$esc_ssid"
	fi
} > "$WPA"

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
