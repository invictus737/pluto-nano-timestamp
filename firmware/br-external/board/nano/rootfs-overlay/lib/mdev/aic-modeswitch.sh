#!/bin/sh
# AIC8800 USB Wi-Fi/Bluetooth adapter in "driver CD" storage mode (a69c:572x, Pandora 1111:1111), called
# by mdev for the USB device (DEVPATH, BUSNUM, DEVNUM, PRODUCT from the uevent). usb-storage ignores
# these IDs (linux patch 0004), so nothing is mounted at boot.
#  1. usb_modeswitch with the standard configuration /etc/usb_modeswitch.d/<vid>:<pid> (D80 sticks
#     switch here).
#  2. If the same device (same sysfs path, bus and device number) is still in storage mode after a
#     short wait, the storage route (some DC sticks time out the raw CBW, error -7): usb-storage is told
#     to take this VID:PID (quirks parameter entry with empty flags, the others kept), the device is
#     re-authorized, its sd disk is ejected as a SCSI disk (never mounted: blockdev.sh skips it) and the
#     quirks parameter is restored. The adapter then re-enumerates in Wi-Fi mode (DC: a69c:88dc).
# Runs in the background (mdev is not blocked), every wait bounded, each step logged (tag aic-modeswitch).
# NEXUS_ROOT prefixes sysfs/proc/run and the configuration directory (tests only); AIC_STEP the poll
# interval in seconds.
P=${NEXUS_ROOT:-}
SYS=$P/sys; RUN=$P/run/aic-modeswitch; CFG=$P/etc/usb_modeswitch.d
Q=$SYS/module/usb_storage/parameters/quirks
STEP=${AIC_STEP:-1}
if [ "${1:-}" != --fg ]; then
	[ -n "${DEVPATH:-}" ] || exit 0
	"$0" --fg < /dev/null > /dev/null 2>&1 &
	exit 0
fi
log() { logger -t aic-modeswitch "$*"; }
D=$SYS$DEVPATH
[ -d "$D" ] || exit 0
VID=$(cat "$D/idVendor" 2>/dev/null); PID=$(cat "$D/idProduct" 2>/dev/null)
[ -n "$VID" ] && [ -n "$PID" ] || exit 0
storage_id() { case "$1:$2" in a69c:572[1-7acf]|1111:1111) return 0 ;; esac; return 1; }
storage_id "$VID" "$PID" || exit 0
BUS=${BUSNUM:-$(cat "$D/busnum" 2>/dev/null)}; DEV=${DEVNUM:-$(cat "$D/devnum" 2>/dev/null)}
n3() { v=$(printf '%s' "$1" | sed 's/^0*//'); printf '%03d' "${v:-0}"; }   # 8, 08, 008 -> 008 (no octal)
BUS=$(n3 "$BUS"); DEV=$(n3 "$DEV")
NAME=${DEVPATH##*/}
# lock DIR: a directory holding the owner's pid; a lock whose owner is gone (killed) is taken over
lock() {
	mkdir "$1" 2>/dev/null && { echo $$ > "$1/pid"; return 0; }
	o=$(cat "$1/pid" 2>/dev/null); [ -n "$o" ] && kill -0 "$o" 2>/dev/null && return 1
	rm -rf "$1"; mkdir "$1" 2>/dev/null && { echo $$ > "$1/pid"; return 0; }; return 1
}
unlock() { rm -rf "$1"; }
# one run per device: a replayed add event (S10mdev) or a re-authorization does not start a second one
mkdir -p "$RUN" && lock "$RUN/dev-$NAME" || exit 0
QSAVED=; QLOCK=
cleanup() {
	# with a newline: a zero-byte write (empty saved list) is ignored by sysfs and would keep the entry
	[ -n "$QSAVED" ] && { printf '%s\n' "$(cat "$RUN/quirks.saved")" > "$Q" && log "$NAME: usb-storage quirks restored ('$(cat "$Q")')"; rm -f "$RUN/quirks.saved"; }
	[ -n "$QLOCK" ] && unlock "$RUN/quirks.lock"; unlock "$RUN/dev-$NAME"
}
trap cleanup EXIT
trap 'exit 1' INT TERM
# still this device in storage mode? (gone or re-enumerated = switched)
same() { [ -d "$D" ] && [ "$(cat "$D/idVendor" 2>/dev/null):$(cat "$D/idProduct" 2>/dev/null)" = "$VID:$PID" ] && \
	[ "$(n3 "$(cat "$D/devnum" 2>/dev/null)")" = "$DEV" ]; }
# wait_gone SECONDS: 0 when the device left storage mode within the time
wait_gone() { n=0; while [ $n -lt "$1" ]; do same || return 0; sleep "$STEP"; n=$((n + STEP)); done; ! same; }

log "$NAME: $VID:$PID storage mode (bus $BUS device $DEV): usb_modeswitch"
if [ -f "$CFG/$VID:$PID" ]; then
	# at most 3 s: a stick that times out the raw CBW (DC, error -7) goes straight to the storage route
	usb_modeswitch -b "$BUS" -g "$DEV" -c "$CFG/$VID:$PID" > "$RUN/out-$NAME" 2>&1 & mp=$!
	n=0; while [ $n -lt 30 ] && kill -0 $mp 2>/dev/null; do usleep 100000; n=$((n + 1)); done
	kill $mp 2>/dev/null; wait $mp; r=$?; out=$(cat "$RUN/out-$NAME" 2>/dev/null); rm -f "$RUN/out-$NAME"
	log "$NAME: usb_modeswitch exit $r: $(printf '%s' "$out" | grep -iE 'error|success|not|gone|found' | tr '\n' ' ' | cut -c1-300)"
else log "$NAME: no $CFG/$VID:$PID"; fi
wait_gone 2 && { log "$NAME: switched (left storage mode)"; exit 0; }

log "$NAME: still $VID:$PID after usb_modeswitch: storage route (usb-storage + SCSI eject)"
[ -w "$Q" ] || { log "$NAME: $Q not writable, giving up"; exit 1; }
# the quirks parameter is shared by every device: one fallback at a time (bounded wait for the other one)
n=0; while ! lock "$RUN/quirks.lock"; do n=$((n + 1)); [ $n -ge 60 ] && { log "$NAME: quirks lock busy, giving up"; exit 1; }; sleep "$STEP"; done
QLOCK=1
OLD=$(cat "$Q" 2>/dev/null)
printf '%s' "$OLD" > "$RUN/quirks.saved"; QSAVED=1
NEW=$(printf '%s' "$OLD" | tr ',' '\n' | grep -v -i "^$VID:$PID:" | grep -v '^$' | tr '\n' ',')
NEW="$NEW$VID:$PID:"
printf '%s\n' "$NEW" > "$Q" || { log "$NAME: quirks write failed"; exit 1; }
log "$NAME: usb-storage quirks '$NEW' (was '$OLD'), re-authorizing the device"
echo 0 > "$D/authorized"; sleep "$STEP"; echo 1 > "$D/authorized"
# the sd disk whose sysfs path lies under this USB device
RD=$(readlink -f "$D")
disk() { for b in "$SYS"/block/sd*; do [ -e "$b" ] || continue; case "$(readlink -f "$b")" in "$RD"/*) basename "$b"; return 0 ;; esac; done; return 1; }
n=0; SD=; while [ $n -lt 20 ]; do SD=$(disk) && break; same || { log "$NAME: device left storage mode while waiting for its disk"; exit 0; }; sleep "$STEP"; n=$((n + STEP)); done
[ -n "$SD" ] || { log "$NAME: no sd disk under $DEVPATH after 20 s, giving up"; exit 1; }
log "$NAME: disk $SD"
# never mounted (blockdev.sh skips the AIC driver CD); unmount whatever got mounted all the same
for m in $(awk -v d="/dev/$SD" 'index($1, d) == 1 { print $2 }' "$P/proc/mounts" 2>/dev/null); do
	umount "$m" 2>/dev/null && log "$NAME: unmounted $m" || log "$NAME: umount $m failed"
done
if eject "/dev/$SD" > /dev/null 2>&1 || eject -s "/dev/$SD" > /dev/null 2>&1; then log "$NAME: $SD ejected"
else log "$NAME: eject $SD failed"; fi
wait_gone 10 && { log "$NAME: switched (left storage mode after the eject)"; exit 0; }
log "$NAME: still $VID:$PID after the eject, giving up"; exit 1
