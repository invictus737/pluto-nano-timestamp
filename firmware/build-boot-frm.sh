#!/usr/bin/env bash
# boot.frm for the stock mass-storage update path (update.sh handle_boot_frm): boot.bin (FSBL + U-Boot)
# + 128 KiB U-Boot environment + the Pluto mtd-info block + md5, the layout plutosdr-fw builds.
# boot.bin is Tezuka's official Nano release image with the STANDARD clocks (no overclock FSBL), the
# environment is uboot/uboot-env-nexus.txt.
# usage: build-boot-frm.sh <plutosdr-fw checkout> <output dir>
set -euo pipefail
FW=$(cd "${1:?plutosdr-fw}" && pwd); OUT=$(mkdir -p "${2:?out}" && cd "$2" && pwd); HERE=$(cd "$(dirname "$0")" && pwd)
TZ_TAG=v0.3.24; TZ_ZIP=tezuka-nano-v0.3.24-a304a1f.zip
TZ_ZIP_SHA=6ddc4af05f07e892cbcc9b08f6a9831f1130706962028c8c83a4587e4e8f7d36
BOOT_SHA=6ae8d83237b6774280839942e0a8cc1a8f2ef5246e158aa5aff3db15aabec479   # sdimg/BOOT.bin, standard clocks
WORK=$HERE/work-tezuka; mkdir -p "$WORK"
if ! echo "$TZ_ZIP_SHA  $WORK/$TZ_ZIP" | sha256sum -c --status 2>/dev/null; then
  curl -sfL -o "$WORK/$TZ_ZIP" "https://github.com/F5OEO/tezuka_fw/releases/download/$TZ_TAG/$TZ_ZIP"
  echo "$TZ_ZIP_SHA  $WORK/$TZ_ZIP" | sha256sum -c
fi
unzip -o -q -j "$WORK/$TZ_ZIP" "${TZ_ZIP%.zip}/sdimg/BOOT.bin" -d "$WORK"
echo "$BOOT_SHA  $WORK/BOOT.bin" | sha256sum -c
# environment: comments stripped, 128 KiB with CRC (the size U-Boot and fw_printenv use)
grep -v '^#' "$HERE/uboot/uboot-env-nexus.txt" > "$OUT/uboot-env-nexus.txt"
! grep -i -E '^(ssid_wlan|pwd_wlan|xo_correction)=' "$OUT/uboot-env-nexus.txt" || { echo "credentials/device values in the env"; exit 1; }
mkenvimage -s 0x20000 -o "$WORK/uboot-env.bin" "$OUT/uboot-env-nexus.txt"
KEY=$FW/scripts/target_mtd_info.key
[ "$(stat -c %s "$KEY")" = 1024 ] || { echo "unexpected $KEY"; exit 1; }
cat "$WORK/BOOT.bin" "$WORK/uboot-env.bin" "$KEY" > "$OUT/boot.frm.body"
md5sum "$OUT/boot.frm.body" | cut -d ' ' -f1 > "$OUT/boot.frm.md5"
cat "$OUT/boot.frm.body" "$OUT/boot.frm.md5" > "$OUT/boot.frm"; rm "$OUT/boot.frm.body"
ls -la "$OUT/boot.frm"; sha256sum "$OUT/boot.frm"
