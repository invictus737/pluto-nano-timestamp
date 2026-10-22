#!/bin/sh
# Host test of nexus-config and the board scripts it drives (nexus-net, nexus-clock, nexus-role,
# apps-flash-lib, S39bluetooth, update.sh's config.txt handling): every nonint function runs against a
# sandbox (NEXUS_ROOT) with fake fw_printenv/fw_setenv, mtd files, ip, wpa_cli, ifup, bluetoothctl,
# nexus2 provision, init scripts. Every fake records its argv; secrets must never appear in any argv,
# the log or the output.
#   firmware/tests/nexus-config-test.sh            needs busybox sh + jq (Linux)
#   firmware/tests/nexus-config-test.sh --docker   runs itself in a throwaway alpine:3.20 container
#                                                  (busybox 1.36.1 like the image; jq installed there)
# STOCK_BOARD_DIR=<dir with ADI buildroot board/pluto S40network update.sh S45msd> also patches those
# with post-build.sh and tests update.sh's config.txt handling (python3 on the host).
HERE=$(cd "$(dirname "$0")" && pwd); FW=$(cd "$HERE/.." && pwd)
OVL=$FW/br-external/board/nano/rootfs-overlay
if [ "${1:-}" = --docker ]; then
	# the files go in through stdin (no bind mounts: they are not shared on every Docker host)
	S=$(mktemp -d); mkdir -p "$S/fw/br-external/board/nano"
	cp -R "$FW/tests" "$S/fw/"; cp -R "$FW/br-external/board/nano/rootfs-overlay" "$S/fw/br-external/board/nano/"
	PBE=
	if [ -n "${STOCK_BOARD_DIR:-}" ] && command -v python3 > /dev/null; then
		mkdir -p "$S/fw/pb"; cp "$STOCK_BOARD_DIR/S40network" "$STOCK_BOARD_DIR/update.sh" "$STOCK_BOARD_DIR/S45msd" "$S/fw/pb/"
		awk '/<<.PY.$/{f=1;n++;next} /^PY$/{f=0} f && n==1' "$FW/br-external/board/nano/post-build.sh" > "$S/patch.py"
		python3 "$S/patch.py" "$S/fw/pb/S40network" "$S/fw/pb/update.sh" "$S/fw/pb/S45msd" || exit 1
		PBE="-e PB_DIR=/fw/pb"
	fi
	COPYFILE_DISABLE=1 tar -C "$S" -cf - fw | docker run --rm -i $PBE -e NEXUS_TEST_CONTAINER=1 alpine:3.20 sh -c 'tar -x -C / &&
		{ apk add -q jq > /dev/null 2>&1 || { echo "jq could not be installed in the container"; exit 1; }; } && exec busybox sh /fw/tests/nexus-config-test.sh'
	r=$?; rm -rf "${S:?}"; exit $r
fi
command -v jq > /dev/null || { echo "needs jq"; exit 1; }
PASS=0; FAILS=0
ok() { PASS=$((PASS + 1)); echo "ok   $*"; }
bad() { FAILS=$((FAILS + 1)); echo "FAIL $*"; }
check() { d=$1; shift; if "$@"; then ok "$d"; else bad "$d"; fi; }
# rc EXPECTED DESC CMD...: the command's exit status must be EXPECTED
rc() { e=$1; d=$2; shift 2; "$@" > $T/out 2> $T/err; r=$?; cat $T/out $T/err >> $T/alloutput; if [ $r = "$e" ]; then ok "$d (exit $r)"; else bad "$d (exit $r, expected $e)"; sed 's/^/     | /' $T/out $T/err; fi; }

T=$(mktemp -d); R=$T/root; B=$T/bin; mkdir -p $R $B
REALJQ=$(command -v jq)
export NEXUS_ROOT=$R PATH=$B:$PATH
SECRETS="Sup3rSecretWiFi correct-horse-battery DmrHotspotPw1 tetraPass9 bmapikey123 svxAuthKey7 OldNetPassword dashPw42"
for d in usr/sbin usr/share/nexus-clock etc/init.d etc/network etc/bluetooth mnt/data/network mnt/data/nexus2 mnt/data/bt/lib \
	mnt/data/dropbear run proc dev sys/class/net/wlan0 sys/class/net/eth0 sys/class/net/usb0 var/run var/log opt/apps tmp \
	sys/bus/iio/devices/iio:device0; do mkdir -p $R/$d; done
cp $OVL/usr/sbin/* $R/usr/sbin/; cp $OVL/etc/init.d/S39bluetooth $R/etc/init.d/; cp $OVL/etc/bluetooth/main.conf $R/etc/bluetooth/
chmod 755 $R/usr/sbin/* $R/etc/init.d/*
echo ad9361-phy > $R/sys/bus/iio/devices/iio:device0/name
echo "device-fw v0.39-nexusBS" > $R/opt/VERSIONS
echo plutosdr > $R/etc/hostname; : > $R/etc/resolv.conf; : > $R/env
echo "ubi0:data /mnt/data ubifs rw,noatime 0 0" > $R/proc/mounts
printf 'auto lo\niface lo inet loopback\n\nauto usb0\niface usb0 inet static\n\taddress 192.168.2.1\n\tnetmask 255.255.255.0\n\nauto usb1\niface usb1 inet dhcp\n\nauto eth0\niface eth0 inet dhcp\n\n' > $R/etc/network/interfaces
printf '1: usb0    inet 192.168.2.1/24 scope global usb0\n3: wlan0    inet 192.168.1.119/24 brd 192.168.1.255 scope global wlan0\n' > $R/ipaddrs
printf 'wpa_state=COMPLETED\nssid=NewNet\nid_str=net1\n' > $R/wpa_status

# ---- fakes: every one records "name args" in $R/argv.log
# argv.log is reset by some tests; argv.all keeps every command line of the run for the secret check
fake() { { echo '#!/bin/sh'; echo 'echo "$(basename "$0") $*" >> "$NEXUS_ROOT/argv.log"; echo "$(basename "$0") $*" >> "$NEXUS_ROOT/argv.all"'; cat; } > $B/$1; chmod 755 $B/$1; }
# jq is the real one, but its command lines are recorded too (secrets go through the environment)
echo "exec $REALJQ \"\$@\"" | fake jq
fake fw_printenv <<'F'
E=$NEXUS_ROOT/env
if [ "$1" = -n ]; then l=$(grep "^$2=" "$E" | head -1); [ -n "$l" ] || { echo "## Error: \"$2\" not defined" >&2; exit 1; }; echo "${l#*=}"; else cat "$E"; fi
F
fake fw_setenv <<'F'
E=$NEXUS_ROOT/env
set1() { k=$1; shift; grep -v "^$k=" "$E" > "$E.t"; [ $# -gt 0 ] && echo "$k=$*" >> "$E.t"; mv "$E.t" "$E"; }
if [ "$1" = -s ]; then while read -r k v; do [ -n "$k" ] && set1 "$k" $v; done < "$2"; else set1 "$@"; fi
F
fake logger <<'F'
shift 2 2>/dev/null; echo "$*" >> "$NEXUS_ROOT/syslog"
F
fake ip <<'F'
case "$*" in
"-4 -o addr show"*) if [ "$5" = dev ]; then grep " $6 " "$NEXUS_ROOT/ipaddrs"; else cat "$NEXUS_ROOT/ipaddrs"; fi ;;
"-4 route show default"*) echo "default via 192.168.1.1 dev wlan0" ;;
esac; exit 0
F
fake wpa_cli <<'F'
case "$*" in *status*) cat "$NEXUS_ROOT/wpa_status" ;; esac; exit 0
F
fake wpa_passphrase <<'F'
IFS= read -r p; echo "network={"; echo "	ssid=\"$1\""; echo "	#psk=\"$p\""; echo "	psk=$(printf '%s%s' "$1" "$p" | sha256sum | cut -c1-64)"; echo "}"
F
for f in ifup ifdown iw udhcpc flash_unlock reboot poweroff; do fake $f < /dev/null; done
fake hostname <<'F'
[ $# = 0 ] && cat "$NEXUS_ROOT/etc/hostname"; exit 0
F
fake bluetoothctl <<'F'
[ "$1" = --timeout ] && shift 2
case "$1" in
show) printf 'Controller 11:22:33:44:55:66 (public)\n\tName: plutosdr\n\tPowered: yes\n\tPairable: yes\n' ;;
paired-devices) echo "Device AA:BB:CC:DD:EE:FF AirPods" ;;
esac; exit 0
F
fake dmesg <<'F'
cat "$NEXUS_ROOT/dmesg" 2>/dev/null; exit 0
F
fake devmem <<'F'
case "$1" in 0xF8000100) echo 0x00028000 ;; 0xF8000120) echo 0x1F000200 ;; 0xF8000104) echo 0x00020000 ;; 0xF8000124) echo 0x0C200000 ;; *) echo 0 ;; esac
F
fake flash_erase <<'F'
n=0; while [ $n -lt $3 ]; do dd if="$NEXUS_ROOT/ff64k" of="$1" bs=65536 seek=$(( $2 / 65536 + n )) count=1 conv=notrunc 2>/dev/null; n=$((n + 1)); done
F
fake flashcp <<'F'
[ $(wc -c < "$1") -le $(wc -c < "$2") ] || exit 1; dd if="$1" of="$2" conv=notrunc 2>/dev/null
F
for s in S23udc S55sdr_ip_gadget S97app S50avahi-daemon S45msd; do
	printf '#!/bin/sh\necho "%s $*" >> "$NEXUS_ROOT/argv.log"; echo "%s $*" >> "$NEXUS_ROOT/argv.all"\n' $s $s > $R/etc/init.d/$s; chmod 755 $R/etc/init.d/$s; done
head -c 65536 /dev/zero | tr '\000' '\377' > $R/ff64k

# ---- mtd sandbox: Tezuka-like boot image (block 0 per clock variant + common rest), env, slot
printf 'dev:    size   erasesize  name\nmtd0: 00100000 00010000 "qspi-fsbl-uboot"\nmtd1: 00020000 00010000 "qspi-uboot-env"\nmtd2: 00040000 00010000 "qspi-nvmfs"\nmtd3: 00100000 00010000 "qspi-linux"\nmtd4: 00100000 00010000 "qspi-bootcopy"\nmtd5: 00080000 00010000 "qspi-apps"\nmtd6: 00080000 00010000 "qspi-data"\n' > $R/proc/mtd
C=$R/usr/share/nexus-clock
# every variant carries the Zynq boot header (width word + XNLX at 0x20), as apps-frm-install checks
for v in standard 495_396 950_600 1100_750; do head -c 65536 /dev/urandom > $C/$v.blk0
	printf '\146\125\231\252\130\116\114\130' | dd of=$C/$v.blk0 bs=1 seek=32 conv=notrunc 2>/dev/null; done
head -c 204800 /dev/urandom > $C/boot-rest.bin; echo "boot_len=$((65536 + 204800))" > $C/layout
(cd $C && sha256sum *.blk0 boot-rest.bin > SHA256SUMS)
ffs() { n=0; while [ $n -lt $2 ]; do cat $R/ff64k; n=$((n + 1)); done > "$1"; }
mkboot() { { cat $C/$1.blk0 $C/boot-rest.bin; n=0; while [ $n -lt 16 ]; do cat $R/ff64k; n=$((n + 1)); done; } | head -c 1048576 > $2; }
mkboot standard $R/dev/mtd0; cp $R/dev/mtd0 $R/dev/mtd4
head -c 131072 /dev/urandom > $R/dev/mtd1; head -c 262144 /dev/urandom > $R/dev/mtd2; ffs $R/dev/mtd3 16
ffs $R/dev/mtd5 8; ffs $R/dev/mtd6 8
blk0() { dd if=$1 bs=65536 count=1 2>/dev/null | sha256sum | cut -c1-64; }
h() { sha256sum < $1 | cut -c1-64; }
NC=$R/usr/sbin/nexus-config
envv() { sed -n "s/^$1=//p" $R/env; }
mode() { ls -ln "$1" | cut -c1-10; }

echo "== mtd sandbox sizes: $(wc -c < $R/dev/mtd0) $(wc -c < $R/dev/mtd4)"
# ================================================================ system
rc 1 "hostname refused: -bad" $NC nonint do_hostname -bad
rc 1 "hostname refused: 64 characters" $NC nonint do_hostname aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
rc 0 "hostname nexus-test" $NC nonint do_hostname nexus-test
check "hostname in env and /etc/hostname" [ "$(envv hostname)" = nexus-test -a "$(cat $R/etc/hostname)" = nexus-test ]
check "avahi restarted" grep -q "S50avahi-daemon start" $R/argv.log
rc 2 "role refused: bogus" $NC nonint do_role bogus
rc 0 "role master" $NC nonint do_role master
check "role master in env, USB composite rebuilt, NexusBS started" sh -c "[ \"\$(sed -n 's/^operating_mode=//p' $R/env)\" = master ] && grep -q 'S23udc start' $R/argv.log && grep -q 'S97app start' $R/argv.log"
check "get role" [ "$($NC get role)" = master ]
check "nexus-role status" sh -c "$R/usr/sbin/nexus-role status | grep -q 'role: master'"
rc 2 "usb_mode refused: usb3" $NC nonint do_usb_mode usb3
rc 0 "usb_mode host (warns)" $NC nonint do_usb_mode host
check "usb_mode host warning" grep -q WARNING $T/out
check "usb_mode in env" [ "$(envv usb_mode)" = host ]
$NC nonint do_usb_mode peripheral > /dev/null
rc 1 "xo refused: 7 digits" $NC nonint do_xo_correction 4000000
rc 1 "xo refused: 500 ppm off" $NC nonint do_xo_correction 40020000
rc 1 "xo refused: not a number" $NC nonint do_xo_correction 4000000x
rc 0 "xo 39999331" $NC nonint do_xo_correction 39999331
check "xo in env" [ "$(envv xo_correction)" = 39999331 ]
check "xo ppm -16.725" [ "$($NC get xo_ppm)" = "-16.725" ]
check "xo output shows both values" grep -q '39999331 Hz = -16.725 ppm' $T/out
check "xo output: formula, sign convention, 438 Hz per ppm, keep [radio] ppm = 0" sh -c "grep -q 'ppm = (xo - 40000000) / 40000000 x 1e6' $T/out && grep -q 'slow crystal tunes the radio low' $T/out && grep -q '1 ppm = 438 Hz at 438 MHz' $T/out && grep -q 'Keep \[radio\] ppm = 0' $T/out"
: > $R/argv.log
rc 0 "xo the same value again: idempotent" $NC nonint do_xo_correction 39999331
check "xo unchanged: no env write" sh -c "grep -q unchanged $T/out && ! grep -q '^fw_setenv' $R/argv.log"
rc 1 "xo refused: +250.025 ppm (40010001)" $NC nonint do_xo_correction 40010001
rc 0 "xo accepted: -250 ppm (39990000)" $NC nonint do_xo_correction 39990000
printf '[station]\ncallsign = "X"\n[radio]\ntransmit = false\nppm = -16.73   # LO only\n' > $R/mnt/data/nexus2/nexus2.toml
rc 0 "xo with a station still on [radio] ppm" $NC nonint do_xo_correction 39999331
check "xo warns about [radio] ppm = -16.73" grep -q 'WARNING: .*\[radio\] ppm = -16.73' $T/out
printf '[radio]\nppm = 0.0\n[x]\nppm = 5\n' > $R/mnt/data/nexus2/nexus2.toml
rc 0 "xo with [radio] ppm = 0" $NC nonint do_xo_correction 39999400
check "no warning for ppm = 0 (another table's ppm ignored)" sh -c "! grep -q WARNING $T/out"
rm -f $R/mnt/data/nexus2/nexus2.toml
rc 0 "xo --now" $NC nonint do_xo_correction 39999400 --now
check "xo applied to the AD9361 sysfs" [ "$(cat $R/sys/bus/iio/devices/iio:device0/xo_correction)" = 39999400 ]
rc 0 "xo default (unset)" $NC nonint do_xo_correction default
check "xo unset" [ "$($NC get xo_correction)" = unset ]
$NC nonint do_xo_correction 39999331 > /dev/null
rc 2 "cpu clock refused: 2000" $NC nonint do_cpu_clock 2000
rc 1 "cpu clock 1100_750 refused without cooling confirmation" $NC nonint do_cpu_clock 1100_750
check "1100 refusal left the env alone" [ -z "$(envv cpu_clock)" ]
rc 1 "nexus-clock set 1100_750 refused without --cooling-confirmed" $R/usr/sbin/nexus-clock set 1100_750
rc 0 "cpu clock 950_600" $NC nonint do_cpu_clock 950_600
check "clock warning mentions DFU/JTAG" grep -q 'DFU/JTAG' $T/out
check "cpu_clock 950_600 in env" [ "$(envv cpu_clock)" = 950_600 ]
check "nexus-clock status JSON" sh -c "$R/usr/sbin/nexus-clock status | jq -e '.configured == \"950_600\" and .installed == \"standard\" and .cpu_mhz == 666' > /dev/null"
# ---- nexus-clock apply (S22clock) on the mtd sandbox
: > $R/argv.log
rc 0 "nexus-clock apply 950_600" $R/usr/sbin/nexus-clock apply
check "block 0 is 950_600, rest unchanged" [ "$(blk0 $R/dev/mtd0)" = "$(h $C/950_600.blk0)" -a "$(dd if=$R/dev/mtd0 bs=65536 skip=1 2>/dev/null | h /dev/stdin)" = "$(dd if=$R/dev/mtd4 bs=65536 skip=1 2>/dev/null | h /dev/stdin)" ]
check "bootcopy written first and equal" [ "$(h $R/dev/mtd4)" = "$(h $R/dev/mtd0)" ]
check "rebooted once" grep -q "^reboot" $R/argv.log
: > $R/argv.log
rc 0 "nexus-clock apply again: identical, no write" $R/usr/sbin/nexus-clock apply
check "no reboot when identical" sh -c "! grep -q reboot $R/argv.log && ! grep -q flash_erase $R/argv.log"
rc 0 "cpu clock 1100_750 with cooling confirmation" $NC nonint do_cpu_clock 1100_750 --cooling-confirmed
check "1100_750 + cooling yes in env" [ "$(envv cpu_clock)" = 1100_750 -a "$(envv cpu_clock_cooling_confirmed)" = yes ]
sed -i 's/^cpu_clock_cooling_confirmed=.*/cpu_clock_cooling_confirmed=no/' $R/env
rc 0 "apply 1100_750 without cooling falls back to standard" $R/usr/sbin/nexus-clock apply
check "block 0 is standard again" [ "$(blk0 $R/dev/mtd0)" = "$(h $C/standard.blk0)" ]
check "1100 refusal logged" grep -q '1100_750 refused' $R/syslog
sed -i 's/^cpu_clock=.*/cpu_clock=495_396/' $R/env; before=$(h $R/dev/mtd0)
cp $R/dev/mtd5 $T/apps.save; dd if=$R/dev/mtd1 of=$R/dev/mtd5 conv=notrunc 2>/dev/null
rc 1 "alias detection: slot reads like the lower-bank env, clock refused" $R/usr/sbin/nexus-clock apply
check "aliased: boot flash untouched" [ "$(h $R/dev/mtd0)" = "$before" ]
cp $T/apps.save $R/dev/mtd5
echo "spi-nor spi0.0: failed to read ear reg" > $R/dmesg
rc 1 "EAR unreadable: clock refused" $R/usr/sbin/nexus-clock apply
check "EAR unreadable: upper_bank_ok says so" sh -c ". $R/usr/sbin/apps-flash-lib.sh; ! upper_bank_ok > $T/ub && grep -q 'register unreadable' $T/ub"
rm -f $R/dmesg
dd if=/dev/urandom of=$R/dev/mtd0 bs=65536 seek=2 count=1 conv=notrunc 2>/dev/null; before=$(h $R/dev/mtd0)
rc 1 "foreign boot image (not Tezuka): clock refused" $R/usr/sbin/nexus-clock apply
check "foreign boot image untouched" [ "$(h $R/dev/mtd0)" = "$before" ]
mkboot standard $R/dev/mtd0; cp $R/dev/mtd0 $R/dev/mtd4
check "upper_bank_ok on a healthy sandbox" sh -c ". $R/usr/sbin/apps-flash-lib.sh; upper_bank_ok"
# ---- a block 0 write that does not verify: the old block back, and not tried again at the next boot
sed -i 's/^cpu_clock=.*/cpu_clock=950_600/' $R/env; before=$(h $R/dev/mtd0)
# dd fake: the new 950_600 block lands corrupted on mtd0 (the write back of the old block works)
cat > $B/dd <<F
#!/bin/sh
for a in "\$@"; do [ "\$a" = "if=$C/950_600.blk0" ] && exec busybox dd if=/dev/urandom of=$R/dev/mtd0 bs=65536 count=1 conv=notrunc 2>/dev/null; done
exec busybox dd "\$@"
F
chmod 755 $B/dd; : > $R/argv.log
rc 1 "nexus-clock apply: block 0 does not verify" $R/usr/sbin/nexus-clock apply
rm -f $B/dd
check "failed write: the previous block 0 written back, bootcopy restored" [ "$(h $R/dev/mtd0)" = "$before" -a "$(h $R/dev/mtd4)" = "$before" ]
check "failed write: no reboot, CRITICAL logged" sh -c "! grep -q '^reboot' $R/argv.log && grep -q 'previous one was written back' $R/syslog"
check "failure marker written" [ -f $R/mnt/data/nexus-clock.failed ]
check "status: apply_failed" sh -c "$R/usr/sbin/nexus-clock status | jq -e '.apply_failed == true' > /dev/null"
: > $R/argv.log
rc 1 "next boot: the failed write is not tried again" $R/usr/sbin/nexus-clock apply
check "not tried again: no erase, no write" sh -c "! grep -q '^flash_erase' $R/argv.log && ! grep -q '^flashcp' $R/argv.log"
check "not tried again: boot flash unchanged" [ "$(h $R/dev/mtd0)" = "$before" ]
rc 0 "nexus-config sets the clock again: the marker goes" $NC nonint do_cpu_clock 950_600
check "marker removed by set" [ ! -f $R/mnt/data/nexus-clock.failed ]
rc 0 "apply after the new setting" $R/usr/sbin/nexus-clock apply
check "950_600 installed" [ "$(blk0 $R/dev/mtd0)" = "$(h $C/950_600.blk0)" ]
# ---- apps-stock-clock.frm: the standard FSBL goes in and cpu_clock follows it (no overclock written back)
printf 'cpu_clock=1100_750\ncpu_clock_cooling_confirmed=yes\n' >> $R/env; sed -i '/^cpu_clock=950_600$/d' $R/env
mkboot 1100_750 $R/dev/mtd0; cp $R/dev/mtd0 $R/dev/mtd4; cp $R/dev/mtd5 $T/apps.save
head -c 5000 /dev/urandom > $T/pl; cat $C/standard.blk0 $C/boot-rest.bin > $T/bb
md() { md5sum < "$1" | cut -c1-32; }
printf 'NEXUSBS-APPS-FRM 1\npayload_size=%s\npayload_md5=%s\nbootbin_size=%s\nbootbin_md5=%s\nbootbin_desc=test standard\nversion=apps-test-2\ncreated=x\n' \
	$(wc -c < $T/pl) $(md $T/pl) $(wc -c < $T/bb) $(md $T/bb) > $T/hdr
head -c $((4096 - $(wc -c < $T/hdr))) /dev/zero >> $T/hdr; cat $T/hdr $T/pl $T/bb > $T/body
{ cat $T/body; printf '%s\n' "$(md $T/body)"; } > $T/apps-stock-clock.frm; mkdir -p $T/msd
rc 0 "apps-frm-install apps-stock-clock.frm over a 1100_750 board" env MSD=$T/msd $R/usr/sbin/apps-frm-install $T/apps-stock-clock.frm
check "apps-stock-clock: standard block 0, bootcopy equal" [ "$(blk0 $R/dev/mtd0)" = "$(h $C/standard.blk0)" -a "$(h $R/dev/mtd4)" = "$(h $R/dev/mtd0)" ]
check "apps-stock-clock: slot written, markers" [ "$(head -c 18 $R/dev/mtd5)" = 'NEXUSBS-APPS-FRM 1' -a -f $T/msd/APPS_SUCCESS -a -f $T/msd/BOOT_UPDATE_SUCCESS ]
check "apps-stock-clock: cpu_clock = standard in the env" [ "$(envv cpu_clock)" = standard ]
: > $R/argv.log
rc 0 "next boot: S22clock leaves the standard FSBL alone" $R/usr/sbin/nexus-clock apply
check "no clock write after apps-stock-clock" sh -c "! grep -q '^flash_erase' $R/argv.log && ! grep -q '^reboot' $R/argv.log"
cp $T/apps.save $R/dev/mtd5; mkboot standard $R/dev/mtd0; cp $R/dev/mtd0 $R/dev/mtd4
sed -i '/^cpu_clock/d' $R/env

# ================================================================ network
printf 'ssid_wlan=OldNet\npwd_wlan=OldNetPassword\ncountry_wlan=RO\n' >> $R/env
: > $R/argv.log
rc 2 "wifi add: no SSID" $NC nonint do_wifi_add
rc 1 "wifi add refused: SSID of 33 bytes" sh -c "echo Sup3rSecretWiFi | $NC nonint do_wifi_add 123456789012345678901234567890123"
rc 1 "wifi add refused: passphrase of 7 characters" sh -c "echo 1234567 | $NC nonint do_wifi_add NewNet"
rc 1 "wifi add refused: 64 characters, not hex" sh -c "printf '%064d\n' 0 | tr 0 g | $NC nonint do_wifi_add NewNet"
rc 1 "wifi add refused: passphrase with a control character" sh -c "printf 'abc\tdefgh\n' | $NC nonint do_wifi_add NewNet"
check "refusals created no network.json" [ ! -f $R/mnt/data/network/network.json ]
rc 0 "wifi add NewNet (passphrase on stdin)" sh -c "echo Sup3rSecretWiFi | $NC nonint do_wifi_add NewNet"
J=$R/mnt/data/network/network.json
check "network.json 0600" [ "$(mode $J)" = "-rw-------" ]
check "config.txt Wi-Fi imported first, NewNet added" sh -c "jq -e '[.wifi.networks[].ssid] == [\"OldNet\", \"NewNet\"] and .wifi.networks[0].priority == 10 and .wifi.networks[1].priority == 0' $J > /dev/null"
check "wpa_supplicant.conf 0600, derived keys only" sh -c "[ \"\$(ls -ln $R/etc/wpa_supplicant.conf | cut -c1-10)\" = -rw------- ] && grep -c '^	psk=[0-9a-f]\{64\}$' $R/etc/wpa_supplicant.conf | grep -qx 2 && ! grep -q 'Sup3rSecretWiFi\|OldNetPassword' $R/etc/wpa_supplicant.conf"
check "wpa_supplicant.conf country RO" grep -qx 'country=RO' $R/etc/wpa_supplicant.conf
check "wlan0 restarted" grep -q '^ifup wlan0' $R/argv.log
check "interfaces: wlan0 manual with the wpa_cli hook" sh -c "grep -q 'iface wlan0 inet manual' $R/etc/network/interfaces && grep -q 'post-up /usr/sbin/nexus-net-wifi-event start wlan0' $R/etc/network/interfaces"
rc 0 "wifi list" $NC get wifi
check "wifi list: no passphrase, default + connected marks" sh -c "! grep -q 'Sup3rSecretWiFi\|OldNetPassword' $T/out && grep -q '\"OldNet\".*default' $T/out && grep -q '\"NewNet\".*connected' $T/out"
rc 0 "wifi default NewNet" $NC nonint do_wifi_default NewNet
check "NewNet now has the highest priority" sh -c "jq -e '.wifi.networks[1].priority == 11' $J > /dev/null"
rc 1 "wifi default: unknown SSID" $NC nonint do_wifi_default Nope
rc 1 "wifi priority refused: -3" $NC nonint do_wifi_priority OldNet -3
rc 1 "wifi priority refused: 5000 (validate)" $NC nonint do_wifi_priority OldNet 5000
rc 0 "wifi priority OldNet 5" $NC nonint do_wifi_priority OldNet 5
rc 0 "wifi update NewNet passphrase" sh -c "echo correct-horse-battery | $NC nonint do_wifi_add NewNet"
check "update kept one NewNet" sh -c "jq -e '[.wifi.networks[] | select(.ssid == \"NewNet\")] | length == 1' $J > /dev/null"
rc 0 "wifi add open network" $NC nonint do_wifi_add "Cafe Free" --open
check "open network: key_mgmt=NONE" grep -q 'key_mgmt=NONE' $R/etc/wpa_supplicant.conf
rc 1 "wifi static refused: 300.1.1.1/24" $NC nonint do_wifi_ipv4 NewNet static 300.1.1.1/24
rc 1 "wifi static refused: prefix /33 (validate)" $NC nonint do_wifi_ipv4 NewNet static 192.168.1.119/33
rc 1 "wifi static refused: bad gateway" $NC nonint do_wifi_ipv4 NewNet static 192.168.1.119/24 192.168.1.
rc 1 "wifi static refused: 4 DNS servers (validate)" $NC nonint do_wifi_ipv4 NewNet static 192.168.1.119/24 192.168.1.1 1.1.1.1,8.8.8.8,9.9.9.9,1.0.0.1
rc 0 "wifi static NewNet 192.168.1.119/24" $NC nonint do_wifi_ipv4 NewNet static 192.168.1.119/24 192.168.1.1 1.1.1.1
check "per-network static file net1.conf" sh -c "grep -qx 'MODE=static' $R/run/nexus-net/wifi/net1.conf && grep -qx 'ADDRESS=192.168.1.119/24' $R/run/nexus-net/wifi/net1.conf"
rc 0 "wifi remove OldNet" $NC nonint do_wifi_remove OldNet
rc 1 "wifi remove: unknown SSID" $NC nonint do_wifi_remove OldNet
check "OldNet gone" sh -c "! jq -e 'any(.wifi.networks[]; .ssid == \"OldNet\")' $J > /dev/null"
rc 2 "eth: bad mode" $NC nonint do_eth_ipv4 manual
rc 0 "eth static 10.0.0.2/24, no gateway" $NC nonint do_eth_ipv4 static 10.0.0.2/24 -
check "interfaces: eth0 static 10.0.0.2 netmask 255.255.255.0" sh -c "grep -A3 'iface eth0 inet static' $R/etc/network/interfaces | grep -q 'netmask 255.255.255.0'"
rc 0 "eth dhcp" $NC nonint do_eth_ipv4 dhcp
check "interfaces: eth0 dhcp with the host name" sh -c "grep -A1 'iface eth0 inet dhcp' $R/etc/network/interfaces | grep -q 'hostname nexus-test'"
rc 1 "dns refused: 1.1.1" $NC nonint do_dns 1.1.1
rc 0 "dns 1.1.1.1,8.8.8.8" $NC nonint do_dns 1.1.1.1,8.8.8.8
check "resolv.conf from the DNS override" sh -c "[ \"\$(cat $R/etc/resolv.conf)\" = \"\$(printf 'nameserver 1.1.1.1\nnameserver 8.8.8.8')\" ]"
check "get dns" [ "$($NC get dns)" = "1.1.1.1,8.8.8.8" ]
rc 0 "dns default" $NC nonint do_dns default
rc 1 "country refused: D1" $NC nonint do_wifi_country D1
rc 0 "country de -> DE" $NC nonint do_wifi_country de
check "country in env and wpa_supplicant.conf" sh -c "[ \"\$(sed -n 's/^country_wlan=//p' $R/env)\" = DE ] && grep -qx 'country=DE' $R/etc/wpa_supplicant.conf && grep -q '^iw reg set DE' $R/argv.log"
rc 0 "--no-apply saves only" $NC nonint --no-apply do_wifi_priority NewNet 20
check "--no-apply: rendered file not changed" sh -c "! grep -q 'priority=20' $R/etc/wpa_supplicant.conf"
rc 0 "do_net_apply" $NC nonint do_net_apply
check "do_net_apply rendered it" grep -q 'priority=20' $R/etc/wpa_supplicant.conf
# ---- rollback: a session over the interface being changed
export SSH_CONNECTION="192.168.1.50 50000 192.168.1.119 22"
rc 3 "change over this session's interface: waits for confirmation" $NC nonint do_wifi_priority NewNet 30
check "pending" [ "$($NC get net_pending)" = yes ]
rc 1 "second change while pending refused" $NC nonint do_wifi_priority NewNet 31
check "refused change left network.json alone" sh -c "jq -e '.wifi.networks[] | select(.ssid == \"NewNet\") | .priority == 30' $J > /dev/null"
rc 0 "do_net_confirm" $NC nonint do_net_confirm
check "confirmed: kept" sh -c "[ \"\$($NC get net_pending)\" = no ] && jq -e '.wifi.networks[] | select(.ssid == \"NewNet\") | .priority == 30' $J.applied > /dev/null"
rc 3 "change with a 2 s rollback, never confirmed" $NC nonint --rollback 2 do_wifi_priority NewNet 40
check "pending before the timer" grep -q 'priority=40' $R/etc/wpa_supplicant.conf
sleep 4
check "rolled back: network.json and wpa_supplicant.conf restored" sh -c "jq -e '.wifi.networks[] | select(.ssid == \"NewNet\") | .priority == 30' $J > /dev/null && grep -q 'priority=30' $R/etc/wpa_supplicant.conf && [ ! -f $R/run/nexus-net/pending ]"
check "rollback logged" grep -q 'rolled back' $R/syslog
rc 1 "confirm after the automatic rollback: too late, says so" $NC nonint do_net_confirm
check "confirm too late: message" grep -q 'already restored' $T/err
rc 1 "roll back with nothing pending: refused (no stale snapshot)" $NC nonint do_net_rollback
check "refused rollback left wpa_supplicant.conf alone" grep -q 'priority=30' $R/etc/wpa_supplicant.conf
rc 3 "change, then rollback now" $NC nonint do_wifi_priority NewNet 45
rc 0 "do_net_rollback" $NC nonint do_net_rollback
check "rollback now restored priority 30" grep -q 'priority=30' $R/etc/wpa_supplicant.conf
export SSH_CONNECTION="192.168.2.10 50000 192.168.2.1 22"
rc 0 "session over usb0: Wi-Fi change applies without rollback" $NC nonint do_wifi_priority NewNet 50
unset SSH_CONNECTION
rc 0 "network status JSON" $NC get network
check "status: hostname + wlan0 address" sh -c "jq -e '.hostname == \"nexus-test\" and .interfaces.wlan0.ipv4 == [\"192.168.1.119/24\"]' $T/out > /dev/null"
rc 0 "hostname change strips network.json hostname" sh -c "jq '.hostname = \"other\"' $J > $J.x && mv $J.x $J && $NC nonint do_hostname nexus-two"
check "network.json has no hostname" sh -c "! jq -e 'has(\"hostname\")' $J > /dev/null"
rc 1 "nexus-net validate: bad schema" sh -c "echo '{\"schema\": 2}' > $T/bad.json && $R/usr/sbin/nexus-net validate $T/bad.json"

# ================================================================ Bluetooth
rm -rf $R/mnt/data/nexus2
rc 0 "bluetooth off before the station exists" $NC nonint do_bluetooth off
check "Player switch off even without a station directory" sh -c "jq -e '.bt_off == true' $R/mnt/data/nexus2/player.json > /dev/null"
check "Player file 0600" [ "$(mode $R/mnt/data/nexus2/player.json)" = -rw------- ]
rm -rf $R/mnt/data/nexus2
rc 0 "S39bluetooth off at a first boot (no station directory)" $R/etc/init.d/S39bluetooth start
check "S39bluetooth created the Player's file" sh -c "jq -e '.bt_off == true' $R/mnt/data/nexus2/player.json > /dev/null"
echo '{"device": "AA:BB:CC:DD:EE:FF", "volume_db": -3, "bt_off": false}' > $R/mnt/data/nexus2/player.json
rc 0 "bluetooth off without bluetoothd: no bluetoothctl call (it would block)" $NC nonint do_bluetooth off
check "no bluetoothctl without bluetoothd" sh -c "! grep -q '^bluetoothctl' $R/argv.log && grep -q 'next boot' $T/out"
echo $$ > $R/var/run/bluetoothd.pid
rc 2 "bluetooth refused: maybe" $NC nonint do_bluetooth maybe
rc 0 "bluetooth off" $NC nonint do_bluetooth off
check "bluetooth off: env, AutoEnable=false, controller powered off" sh -c "grep -qx 'bluetooth=off' $R/env && grep -qx 'AutoEnable=false' $R/etc/bluetooth/main.conf && grep -q '^bluetoothctl --timeout 5 power off' $R/argv.log"
check "Player switch follows, its other settings kept" sh -c "jq -e '.bt_off == true and .device == \"AA:BB:CC:DD:EE:FF\" and .volume_db == -3' $R/mnt/data/nexus2/player.json > /dev/null"
sed -i 's/^AutoEnable=.*/AutoEnable=true/' $R/etc/bluetooth/main.conf
echo '{"bt_off": false}' > $R/mnt/data/nexus2/player.json
rc 0 "S39bluetooth at boot with bluetooth=off" $R/etc/init.d/S39bluetooth start
check "boot: AutoEnable=false, Player switch off" sh -c "grep -qx 'AutoEnable=false' $R/etc/bluetooth/main.conf && jq -e '.bt_off == true' $R/mnt/data/nexus2/player.json > /dev/null"
rc 0 "bluetooth on" $NC nonint do_bluetooth on
check "bluetooth on: env, AutoEnable=true, Player switch on" sh -c "grep -qx 'bluetooth=on' $R/env && grep -qx 'AutoEnable=true' $R/etc/bluetooth/main.conf && jq -e '.bt_off == false' $R/mnt/data/nexus2/player.json > /dev/null"
rc 0 "bluetooth status" $NC get bt
check "bluetooth status shows controller and paired device" sh -c "grep -q 'controller: 11:22:33:44:55:66' $T/out && grep -q 'Powered: yes' $T/out && grep -q 'AirPods' $T/out"

# ================================================================ NexusBS station
rc 1 "station set without an apps slot" $NC nonint do_station_set CALLSIGN YO3TCO
mkdir -p $R/opt/apps/nexus/bin $R/opt/apps/nexus/defaults
printf 'NEXUSBS-APPS-FRM 1\nversion=apps-test-1\ncreated=2026-10-22\n' > $R/opt/apps/.slot-header
cat > $R/opt/apps/nexus/bin/nexus2 <<'F'
#!/bin/sh
echo "nexus2 $*" >> "$NEXUS_ROOT/argv.log"; echo "nexus2 $*" >> "$NEXUS_ROOT/argv.all"
[ "$1" = provision ] || exit 2; D=$3; S=$D/setup.txt
[ -f "$S" ] || { echo "provision: nexus2.toml is valid"; exit 0; }
ls -ln "$S" | cut -c1-10 > "$NEXUS_ROOT/setup.mode"; cp "$S" "$NEXUS_ROOT/setup.seen"
if grep -q '^TETRA_ISSI=bad' "$S"; then echo "refused, nothing changed: TETRA_ISSI must be digits" > $D/setup.result
else echo "applied: $(cut -d= -f1 "$S" | tr '\n' ',' | sed 's/,$//; s/,/, /g')" > $D/setup.result
	grep -q '^XO_CORRECTION=' "$S" && sed -n 's/^XO_CORRECTION=//p' "$S" > $D/xo_correction.pending; fi
rm -f "$S"; echo "provision: setup.txt done"
F
chmod 755 $R/opt/apps/nexus/bin/nexus2
printf '[station]\ncallsign = "YO3TCO"\n' > $R/mnt/data/nexus2/nexus2.toml
rc 1 "station: unknown key" $NC nonint do_station_set FOO bar
rc 1 "station: secret as an argument refused" $NC nonint do_station_set DMR_PASSWORD DmrHotspotPw1
check "refused secret never reached a file" [ ! -f $R/setup.seen ]
rc 0 "station CALLSIGN YO3TCO" $NC nonint do_station_set callsign YO3TCO
check "setup.txt was 0600 and is gone" sh -c "[ \"\$(cat $R/setup.mode)\" = -rw------- ] && [ ! -f $R/mnt/data/nexus2/setup.txt ]"
check "setup.result shown" grep -q 'applied: CALLSIGN' $T/out
check "provision called with the slot defaults" grep -q "nexus2 provision --data $R/mnt/data/nexus2 --defaults $R/opt/apps/nexus/defaults" $R/argv.log
rc 0 "station DMR_PASSWORD on stdin" sh -c "echo DmrHotspotPw1 | $NC nonint do_station_set DMR_PASSWORD"
check "secret reached setup.txt only" grep -qx 'DMR_PASSWORD=DmrHotspotPw1' $R/setup.seen
rc 0 "station setup from stdin (several keys, comments, CRLF, XO)" sh -c "printf '# comment\r\nTETRA_ISSI=2260001\r\nTETRA_PASSWORD=tetraPass9\nBM_API_KEY=bmapikey123\nSVXLINK_AUTH_KEY=svxAuthKey7\nDASHBOARD_PASSWORD=dashPw42\n\nXO_CORRECTION=39999500\n' | $NC nonint do_station_setup"
check "setup lines normalised" sh -c "grep -qx 'TETRA_ISSI=2260001' $R/setup.seen && ! grep -q \"\$(printf '\r')\" $R/setup.seen"
check "XO_CORRECTION from the setup went to the env" [ "$(envv xo_correction)" = 39999500 ]
rc 1 "station setup refused by provision" sh -c "echo TETRA_ISSI=bad | $NC nonint do_station_setup"
check "refusal shown" grep -q 'refused' $T/out
rc 1 "station setup: garbage line" sh -c "echo 'not a setting' | $NC nonint do_station_setup"
rc 0 "get station" $NC get station
check "station status: slot, callsign, setup result" sh -c "grep -q 'apps-test-1' $T/out && grep -q 'callsign: YO3TCO' $T/out && grep -q 'last setup:' $T/out"
rc 0 "get station_login" $NC get station_login
$NC nonint do_role slave > /dev/null
rc 1 "station restart refused in the slave role" $NC nonint do_station_restart
$NC nonint do_role master > /dev/null; : > $R/argv.log
# the init script starts a long-lived child (like S97app's supervisor): it must not inherit the lock
cat > $R/etc/init.d/S97app <<F
#!/bin/sh
echo "S97app \$*" >> "$R/argv.log"; echo "S97app \$*" >> "$R/argv.all"
sleep 30 & echo \$! > $T/daemon.pid
F
rc 0 "station restart" $NC nonint do_station_restart
check "S97app restart called" grep -qx 'S97app restart' $R/argv.log
check "the lock is free while the started daemon runs" sh -c "kill -0 \$(cat $T/daemon.pid) && flock -n $R/run/nexus-config.lock true"
kill $(cat $T/daemon.pid) 2>/dev/null
printf '#!/bin/sh\necho "S97app $*" >> "$NEXUS_ROOT/argv.log"; echo "S97app $*" >> "$NEXUS_ROOT/argv.all"\n' > $R/etc/init.d/S97app
printf 'line1\nline2\nline3\n' > $R/var/log/nexus.log
rc 0 "station log tail" $NC nonint do_station_log 2
check "log tail 2 lines" [ "$(cat $T/out)" = "$(printf 'line2\nline3')" ]

# ================================================================ maintenance
rc 0 "versions" $NC get versions
check "versions: firmware, slot, FSBL variant" sh -c "grep -q 'v0.39-nexusBS' $T/out && grep -q 'apps-test-1' $T/out && grep -q 'standard installed' $T/out"
mkdir -p $R/media/usb
rc 0 "backup to a directory" $NC nonint do_backup /media/usb
f=$(ls $R/media/usb/nexus-data-*.tar.gz)
check "backup 0600 and holds network.json" sh -c "[ \"\$(ls -ln $f | cut -c1-10)\" = -rw------- ] && tar -tzf $f | grep -q 'network/network.json'"
rc 1 "backup inside /mnt/data refused" $NC nonint do_backup /mnt/data/x.tar.gz
rc 1 "backup to a relative path refused" $NC nonint do_backup x.tar.gz
ln -s $R/mnt/data/network $R/media/into-data
rc 1 "backup through a link into /mnt/data refused" $NC nonint do_backup /media/into-data
check "nothing written into /mnt/data" sh -c "! ls $R/mnt/data/network | grep -q nexus-data"
rc 0 "backup to stdout (pipe)" sh -c "$NC nonint do_backup - | gzip -t"
rc 2 "factory reset without the token" $NC nonint do_factory_reset
rc 2 "factory reset with a wrong token" $NC nonint do_factory_reset yes
check "nothing erased yet" [ -f $J ]
rc 0 "factory reset ERASE-ALL-DATA" $NC nonint do_factory_reset ERASE-ALL-DATA
check "data erased, base directories back" sh -c "[ ! -e $J ] && [ ! -e $R/mnt/data/nexus2 ] && [ -d $R/mnt/data/bt/lib ] && [ -d $R/mnt/data/dropbear ] && [ -d $R/mnt/data/network ]"
echo "tmpfs /mnt/data tmpfs rw 0 0" > $R/proc/mounts
rc 1 "factory reset refused when /mnt/data is not the flash partition" $NC nonint do_factory_reset ERASE-ALL-DATA
echo "ubi0:data /mnt/data ubifs rw,noatime 0 0" > $R/proc/mounts
rc 2 "unknown function" $NC nonint do_everything
rc 2 "unknown get item" $NC get nothing
rc 0 "get all" $NC get all
rc 0 "help" $NC help
rc 2 "menu without a terminal" sh -c "$NC < /dev/null > /dev/null"

# ================================================================ update.sh (config.txt on the USB drive)
if [ -n "${PB_DIR:-}" ] && [ -f "$PB_DIR/update.sh" ]; then
	# process_ini of the patched stock update.sh with real paths (throwaway container only)
	mkdir -p /opt /mnt/msd
	printf 'hostname=nexus-two\noperating_mode=master\nusb_mode=auto\ncpu_clock=950_600\ncpu_clock_cooling_confirmed=no\nbluetooth=off\nxo_correction=39999331\ncountry_wlan=DE\nqspiboot=1\n' > $R/env
	# the drive's copy (written at boot, before nexus-config changed the role, clock and Bluetooth)
	printf '[NETWORK]\r\nhostname = nexus-two\r\n[WLAN]\r\nssid_wlan = \r\npwd_wlan = \r\nipaddr_wlan = \r\ncountry_wlan = DE\r\n[USB_ETHERNET]\r\nipaddr_eth = \r\nnetmask_eth = 255.255.255.0\r\n[SYSTEM]\r\nxo_correction = 39999331\r\noperating_mode = slave\r\nusb_mode = peripheral\r\ncpu_clock = standard\r\ncpu_clock_cooling_confirmed = no\r\nbluetooth = on\r\n[ACTIONS]\r\nreset = 0\r\n' > /opt/config.drive
	# the operator edits only the host name on the drive
	sed 's/^hostname = .*/hostname = edited\r/' /opt/config.drive > /mnt/msd/config.txt
	awk '/^ini_parser\(\) \{/,/^\}/' $PB_DIR/update.sh > $T/upd.sh; awk '/^process_ini\(\) \{/,/^\}/' $PB_DIR/update.sh >> $T/upd.sh
	( flash_indication_on() { :; }; flash_indication_off() { :; }; reset() { :; }; dfu() { :; }; . $T/upd.sh; process_ini /mnt/msd/config.txt ) > /dev/null 2>&1
	check "update.sh: the edited key is applied" [ "$(envv hostname)" = edited ]
	check "update.sh: unedited keys keep the nexus-config values" sh -c "grep -qx operating_mode=master $R/env && grep -qx usb_mode=auto $R/env && grep -qx cpu_clock=950_600 $R/env && grep -qx bluetooth=off $R/env"
	check "update.sh: drive copy updated" grep -q 'hostname = edited' /opt/config.drive
	sed -i 's/^bluetooth=.*/bluetooth=on/' $R/env
	sed 's/^cpu_clock = .*/cpu_clock = 1100_750\r/; s/^bluetooth = .*/bluetooth = off\r/' /opt/config.drive > /mnt/msd/config.txt
	( flash_indication_on() { :; }; flash_indication_off() { :; }; reset() { :; }; dfu() { :; }; . $T/upd.sh; process_ini /mnt/msd/config.txt ) > /dev/null 2>&1
	check "update.sh: 1100_750 without cooling keeps the previous clock" sh -c "grep -qx cpu_clock=950_600 $R/env"
	check "update.sh: bluetooth off edited on the drive" sh -c "grep -qx bluetooth=off $R/env"
	check "S40network writes the bluetooth key" grep -q 'bluetooth = \$BT_STATE' $PB_DIR/S40network
	# 1100_750 set without the cooling confirmation installs standard; confirming it on the drive changes
	# what S22clock installs: reboot
	sed -i 's/^cpu_clock=.*/cpu_clock=1100_750/; s/^cpu_clock_cooling_confirmed=.*/cpu_clock_cooling_confirmed=no/' $R/env
	sed 's/^cpu_clock = .*/cpu_clock = 1100_750\r/' /opt/config.drive > $T/drive && cp $T/drive /opt/config.drive
	sed 's/^cpu_clock_cooling_confirmed = .*/cpu_clock_cooling_confirmed = yes\r/' /opt/config.drive > /mnt/msd/config.txt
	( flash_indication_on() { :; }; flash_indication_off() { :; }; reset() { :; }; dfu() { :; }; do_reset=0; . $T/upd.sh; process_ini /mnt/msd/config.txt; echo "$do_reset" > $T/do_reset ) > /dev/null 2>&1
	check "update.sh: confirming the cooling of 1100_750 reboots to apply it" sh -c "grep -qx cpu_clock_cooling_confirmed=yes $R/env && [ \"\$(cat $T/do_reset)\" = 1 ]"
	sed 's/^bluetooth = .*/bluetooth = on\r/' /opt/config.drive > /mnt/msd/config.txt
	( flash_indication_on() { :; }; flash_indication_off() { :; }; reset() { :; }; dfu() { :; }; do_reset=0; . $T/upd.sh; process_ini /mnt/msd/config.txt; echo "$do_reset" > $T/do_reset ) > /dev/null 2>&1
	check "update.sh: an edit that leaves the installed clock alone does not reboot" [ "$(cat $T/do_reset)" = 0 ]
else echo "skip update.sh tests (STOCK_BOARD_DIR not given)"; fi

# ================================================================ S97app supervisor (container only: real /opt, /var/run)
if [ -n "${NEXUS_TEST_CONTAINER:-}" ]; then
	mkdir -p /opt/apps/nexus /mnt/data/nexus2 /var/run /var/log
	printf '#!/bin/sh\necho "$$ $APPS_DIR $DATA_DIR $(pwd)" >> /tmp/start.calls\nexec sleep 1000\n' > /opt/apps/nexus/start.sh
	chmod 755 /opt/apps/nexus/start.sh; rm -f /tmp/start.calls
	sed -i '/^operating_mode=/d' $R/env; echo operating_mode=master >> $R/env
	S97=$OVL/etc/init.d/S97app
	( exec 9> $T/caller.lock; flock -n 9 && $S97 start > $T/s97.out 2>&1 )
	sleep 1
	check "S97app: start.sh runs with APPS_DIR/DATA_DIR in /mnt/data/nexus2" grep -q ' /opt/apps /mnt/data /mnt/data/nexus2$' /tmp/start.calls
	check "S97app: the supervisor does not hold its caller's lock" flock -n $T/caller.lock true
	rc 0 "S97app start again: idempotent" $S97 start
	check "S97app: already running, not started twice" sh -c "grep -q 'already running' $T/out && [ \$(wc -l < /tmp/start.calls) = 1 ]"
	child=$(cut -d' ' -f1 /tmp/start.calls); sup=$(cat /var/run/nexus.pid)
	rc 0 "S97app stop" $S97 stop
	check "S97app stop: supervisor and child gone (nothing orphaned)" sh -c "! kill -0 $child 2>/dev/null && ! kill -0 $sup 2>/dev/null"
	kill $child 2>/dev/null
	# the child exits by itself: started again after the fixed 15 s, not at once
	printf '#!/bin/sh\necho "$$" >> /tmp/start.calls\nexit 3\n' > /opt/apps/nexus/start.sh; rm -f /tmp/start.calls
	$S97 start > /dev/null; sleep 3
	check "S97app: an exiting child is not restarted in a tight loop" [ "$(wc -l < /tmp/start.calls)" = 1 ]
	$S97 stop > /dev/null
	check "S97app: log records the exit" grep -q 'exit 3' /var/log/nexus.log
	sed -i '/^operating_mode=/d' $R/env
else echo "skip S97app supervisor tests (container only: NEXUS_TEST_CONTAINER)"; fi

# ================================================================ secrets
leak=0
for s in $SECRETS; do
	for f in $R/argv.all $R/syslog $T/alloutput; do grep -q -- "$s" $f 2>/dev/null && { echo "     secret '$s' found in $f"; leak=1; }; done
done
check "no secret in any command line, the log or the output" [ $leak = 0 ]
check "the secret check saw the command lines" sh -c "grep -q '^wpa_passphrase NewNet' $R/argv.all && grep -q '^jq ' $R/argv.all"
echo "== $PASS passed, $FAILS failed"
if [ -n "${NEXUS_TEST_KEEP:-}" ]; then echo "sandbox kept: $T"; else rm -rf "${T:?}"; fi
[ $FAILS = 0 ]
