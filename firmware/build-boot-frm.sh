#!/usr/bin/env bash
# boot*.frm for the mass-storage update path (update.sh handle_boot_frm): boot.bin (FSBL + U-Boot)
# + 128 KiB U-Boot environment + the Pluto mtd-info block + md5, the layout plutosdr-fw builds.
# boot.bin: Tezuka v0.3.24 Nano images (tezuka-boot.sh), environment: uboot/uboot-env-nexus.txt.
#   boot.frm                 standard clocks
#   boot-cpu1100-ddr750.frm  FSBL with CPU 1100 MHz / DDR 750 MT/s: tested on the reference board only (HamGeek Pluto Nano Rev.B) WITH HEATSINKS AND A FAN INSTALLED, ran for days; not a Tezuka-published test result; above the -1 speed grade spec — do not use without active cooling
# All share U-Boot and environment; the build checks they differ from boot.frm only in FSBL bytes of
# the first 64 KiB block. The STOCK update.sh only looks for the name boot.frm.
# usage: build-boot-frm.sh <plutosdr-fw checkout or -> <output dir>
set -euo pipefail
FW=${1:?plutosdr-fw or -}; OUT=$(mkdir -p "${2:?out}" && cd "$2" && pwd); HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/tezuka-boot.sh"
W=$HERE/work-tezuka; mkdir -p "$W"
KEY_SHA=bf76f9bf4369d80325701a9066ec43a0cf69f48050f1770ee2bc5c9066786a34
KEY=$W/target_mtd_info.key
if [ "$FW" != - ] && [ -f "$FW/scripts/target_mtd_info.key" ]; then cp "$FW/scripts/target_mtd_info.key" "$KEY"
else curl -sfL -o "$KEY" https://raw.githubusercontent.com/analogdevicesinc/plutosdr-fw/9e90bce43f849882cd43c20a08effca132790fb3/scripts/target_mtd_info.key; fi
echo "$KEY_SHA  $KEY" | sha256sum -c --status || { echo "mtd-info key checksum mismatch"; exit 1; }
# environment: comments stripped, 128 KiB with CRC (the size U-Boot and fw_printenv use)
grep -v '^#' "$HERE/uboot/uboot-env-nexus.txt" > "$OUT/uboot-env-nexus.txt"
! grep -i -E '^(ssid_wlan|pwd_wlan|xo_correction)=' "$OUT/uboot-env-nexus.txt" || { echo "credentials/device values in the env"; exit 1; }
mkenvimage -s 0x20000 -o "$W/uboot-env.bin" "$OUT/uboot-env-nexus.txt"
for v in $TZ_VARIANTS; do
	tezuka_boot "$v" "$W/boot-$v.bin"
	case $v in std) n=boot.frm ;; *) n=boot-$v.frm ;; esac
	cat "$W/boot-$v.bin" "$W/uboot-env.bin" "$KEY" > "$W/body"
	{ cat "$W/body"; md5sum "$W/body" | cut -d' ' -f1; } > "$OUT/$n"
	echo "$n: $TZ_DESC, $(stat -c %s "$OUT/$n") bytes"
done
# variants: same length, differences only inside the first 64 KiB (FSBL, ps7_init) and in the trailer
python3 -I - "$OUT" <<'PY'
import sys, os
d = sys.argv[1]; ref = open(os.path.join(d, 'boot.frm'), 'rb').read()
for n in ('boot-cpu1100-ddr750.frm',):
    b = open(os.path.join(d, n), 'rb').read()
    assert len(b) == len(ref), n
    diff = [i for i in range(len(ref) - 33) if ref[i] != b[i]]
    assert diff and diff[-1] < 0x10000, (n, hex(diff[-1]) if diff else None)
    print("%s: %d bytes differ from boot.frm, all in 0x%x..0x%x (FSBL), plus the md5 trailer" % (n, len(diff), diff[0], diff[-1]))
PY
