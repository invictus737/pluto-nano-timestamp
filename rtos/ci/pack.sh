#!/usr/bin/env bash
# Packs one RTEMS ELF with the bitstream into a FIT (config-1 kernel only, config-2 + bitstream).
# usage: rtos/ci/pack.sh <elf> <system_top.xsa> <out .itb>
set -euo pipefail
ELF=${1:?elf}; XSA=${2:?xsa}; ITB=${3:?out itb}; HERE=$(cd "$(dirname "$0")/.." && pwd)
OBJCOPY=${OBJCOPY:-${RTEMS_TOOLS:-/opt/rtems/6}/bin/arm-rtems6-objcopy}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
BIN=$T/nexus-rtos.bin; "$OBJCOPY" -O binary "$ELF" "$BIN"
# Load at the lowest load address (the vector table), enter at the ELF entry (_start); the
# binary must stay compact (an initialised section in the high non-cacheable region would
# blow it up to ~500 MB).
ENTRY=$(readelf -h "$ELF" | awk '/Entry point/ {print $4}')
LOAD=$(readelf -lW "$ELF" | awk '$1=="LOAD" && $5!="0x00000" {print $3}' | sort | head -1)
SIZE=$(stat -c %s "$BIN")
echo "$(basename "$ITB"): load $LOAD entry $ENTRY binary $SIZE bytes"
[ $((ENTRY)) -ge $((LOAD)) ] && [ $((ENTRY)) -lt $((LOAD + 0x1000)) ] || { echo "entry not at the start of the image"; readelf -lW "$ELF"; exit 1; }
[ "$SIZE" -lt $((48 * 1024 * 1024)) ] || { echo "binary too large"; readelf -lW "$ELF"; exit 1; }
unzip -q -o "$XSA" system_top.bit -d "$T"
dtc -I dts -O dtb -o "$T/stub.dtb" "$HERE/image/stub.dts"
# gzip kernel (PACK_GZIP=0 for none): half the bytes to read from QSPI at boot and to load
# over the serial console; U-Boot decompresses it to the load address
KDATA=nexus-rtos.bin; KCOMP=none
if [ "${PACK_GZIP:-1}" = 1 ]; then gzip -9 -n -c "$BIN" > "$T/nexus-rtos.bin.gz"; KDATA=nexus-rtos.bin.gz; KCOMP=gzip; fi
sed -e "s/@LOAD@/$LOAD/g" -e "s/@ENTRY@/$ENTRY/g" -e "s/@KDATA@/$KDATA/g" -e "s/@KCOMP@/$KCOMP/g" "$HERE/image/rtos.its.in" > "$T/rtos.its"
(cd "$T" && mkimage -f rtos.its "$ITB" >/dev/null)
mkimage -l "$ITB" | grep -E "Data Size|Configuration|FIT description" | head -12
