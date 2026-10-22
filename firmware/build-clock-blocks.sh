#!/usr/bin/env bash
# CPU/DDR clock variants for nexus-clock: the first 64 KiB block (FSBL with its ps7_init PLL data) of
# each Tezuka v0.3.24 Nano boot image, plus the rest of the standard BOOT.bin, into the rootfs overlay
# (/usr/share/nexus-clock). The build is refused unless every variant equals the standard image outside
# block 0 (same U-Boot, same layout).
# usage: build-clock-blocks.sh <generated rootfs overlay>
set -euo pipefail
OVL=$(mkdir -p "${1:?overlay}" && cd "$1" && pwd); HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/tezuka-boot.sh"
D=$OVL/usr/share/nexus-clock; rm -rf "$D"; mkdir -p "$D"
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
for v in $TZ_VARIANTS; do tezuka_boot "$v" "$W/$v.bin"; done
L=$(stat -c %s "$W/std.bin"); [ "$L" -gt 65536 ] || { echo "standard boot image too small"; exit 1; }
tail -c +65537 "$W/std.bin" > "$D/boot-rest.bin"
for v in $TZ_VARIANTS; do
	[ "$(stat -c %s "$W/$v.bin")" = "$L" ] || { echo "$v: length differs from the standard image"; exit 1; }
	cmp -s <(tail -c +65537 "$W/$v.bin") "$D/boot-rest.bin" || { echo "$v: differs from the standard image outside block 0"; exit 1; }
	case $v in std) n=standard ;; cpu495-ddr396) n=495_396 ;; cpu950-ddr600) n=950_600 ;; cpu1100-ddr750) n=1100_750 ;; esac
	head -c 65536 "$W/$v.bin" > "$D/$n.blk0"
	echo "$n: $(cmp -l "$W/$v.bin" "$W/std.bin" | wc -l) bytes differ from standard, all in block 0"
done
(cd "$D" && sha256sum *.blk0 boot-rest.bin > SHA256SUMS)
echo "boot_len=$L" > "$D/layout"
cat "$D/SHA256SUMS"
