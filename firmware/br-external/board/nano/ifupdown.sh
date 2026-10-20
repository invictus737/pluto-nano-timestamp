#!/bin/sh
# mdev helper for network interfaces (replaces the stock board/pluto/ifupdown.sh): wlan*, eth*,
# usb*. Any USB Wi-Fi or Ethernet adapter plugged at any time (AIC8800, Realtek r8188eu/rtl8xxxu,
# r8152 RTL8153/RTL8156, ASIX, CDC) is brought up from /etc/network/interfaces (S40network writes
# eth0 and, with config.txt [WLAN], wlan0). DHCP keeps retrying in the background: Wi-Fi association
# often finishes after ifup's own udhcpc has given up. Idempotent, safe to call more than once.
IF=${MDEV:?}; PIDF=/var/run/udhcpc.$IF.pid
has_ip() { ip -4 -o addr show dev "$IF" 2>/dev/null | grep -q ' inet '; }
is_dhcp() { awk -v i="$IF" '$1 == "iface" && $2 == i && $4 == "dhcp" {f=1} END {exit !f}' /etc/network/interfaces; }
case "${ACTION}" in
add|"")
	mkdir /var/run/ifupdown.$IF 2>/dev/null || exit 0
	trap 'rmdir /var/run/ifupdown.$IF' EXIT
	ip link set "$IF" up 2>/dev/null
	has_ip || ifup "$IF" > /dev/null 2>&1
	if ! has_ip && is_dhcp; then
		[ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null || udhcpc -R -b -p "$PIDF" -i "$IF" > /dev/null 2>&1
	fi
	echo $(ip -f inet -o addr show "$IF" | cut -d\  -f 7 | cut -d/ -f 1) > /opt/ipaddr-$IF
	logger -t ifupdown "$IF up ($(cat /opt/ipaddr-$IF))"
	;;
remove)
	[ -f "$PIDF" ] && kill "$(cat "$PIDF")" 2>/dev/null; rm -f "$PIDF"
	ifdown -f "$IF" > /dev/null 2>&1
	;;
esac
exit 0
