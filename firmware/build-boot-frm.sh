#!/usr/bin/env bash
# boot*.frm for the mass-storage update path (update.sh handle_boot_frm): boot.bin (FSBL + U-Boot)
# + 128 KiB U-Boot environment + the Pluto mtd-info block + md5, the layout plutosdr-fw builds.
# boot.bin: the Tezuka v0.3.24 Nano BOOT.bin with the standard clocks (the CPU/DDR clock is a setting,
# config.txt cpu_clock / nexus-clock, not a separate boot file); environment: uboot/uboot-env-nexus.txt.
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
for v in std; do
	tezuka_boot "$v" "$W/boot-$v.bin"
	case $v in std) n=boot.frm ;; *) n=boot-$v.frm ;; esac
	cat "$W/boot-$v.bin" "$W/uboot-env.bin" "$KEY" > "$W/body"
	{ cat "$W/body"; md5sum "$W/body" | cut -d' ' -f1; } > "$OUT/$n"
	echo "$n: $TZ_DESC, $(stat -c %s "$OUT/$n") bytes"
done
