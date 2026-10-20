#!/bin/sh
# mdev helper for ttyACM*: the Tetra Amp Plus controller (RP2040, pico-sdk USB CDC, VID 2e8a) gets
# the stable name /dev/tetra-amp. Other CDC-ACM devices (a TETRA radio's PEI, modems) are left alone.
u=$(readlink -f /sys/class/tty/$MDEV/device/..)
case "$ACTION" in
add|"")
	[ "$(cat $u/idVendor 2>/dev/null)" = 2e8a ] || exit 0
	ln -sf $MDEV /dev/tetra-amp
	logger -t tetra-amp "controller on /dev/$MDEV ($(cat $u/product 2>/dev/null))" ;;
remove)
	[ "$(readlink /dev/tetra-amp)" = $MDEV ] && rm -f /dev/tetra-amp ;;
esac
exit 0
