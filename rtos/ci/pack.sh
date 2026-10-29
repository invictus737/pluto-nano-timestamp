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
# Boot limits (rtos/QSPI.md): U-Boot copies the FIT to fit_load_address (env, 0x2080000) and
# gunzips the kernel to LOAD, which must end below the FIT copy; CONFIG_SYS_BOOTM_LEN (0x3C00000)
# caps the decompressed size; the QSPI slot 0x200000..0x2000000 holds at most 30 MiB of FIT.
FIT_LOAD_ADDRESS=${FIT_LOAD_ADDRESS:-0x2080000}; BOOTM_LEN=0x3C00000; FIT_SLOT=0x1E00000
KMAX=$((FIT_LOAD_ADDRESS - LOAD)); [ "$KMAX" -le $((BOOTM_LEN)) ] || KMAX=$((BOOTM_LEN))
[ "$SIZE" -le "$KMAX" ] || { echo "binary $SIZE bytes overlaps the FIT copy at $FIT_LOAD_ADDRESS (max $KMAX)"; readelf -lW "$ELF"; exit 1; }
unzip -q -o "$XSA" system_top.bit -d "$T"
dtc -I dts -O dtb -o "$T/stub.dtb" "$HERE/image/stub.dts"
# gzip kernel (PACK_GZIP=0 for none): half the bytes to read from QSPI at boot and to load
# over the serial console; U-Boot decompresses it to the load address
KDATA=nexus-rtos.bin; KCOMP=none
if [ "${PACK_GZIP:-1}" = 1 ]; then gzip -9 -n -c "$BIN" > "$T/nexus-rtos.bin.gz"; KDATA=nexus-rtos.bin.gz; KCOMP=gzip; fi
sed -e "s/@LOAD@/$LOAD/g" -e "s/@ENTRY@/$ENTRY/g" -e "s/@KDATA@/$KDATA/g" -e "s/@KCOMP@/$KCOMP/g" "$HERE/image/rtos.its.in" > "$T/rtos.its"
(cd "$T" && mkimage -f rtos.its "$ITB" >/dev/null)
ITB_SIZE=$(stat -c %s "$ITB")
echo "$(basename "$ITB"): FIT $ITB_SIZE bytes (slot $((FIT_SLOT))), kernel $SIZE of $KMAX bytes"
[ "$ITB_SIZE" -le $((FIT_SLOT)) ] || { echo "FIT larger than the 30 MiB QSPI slot"; rm -f "$ITB"; exit 1; }
mkimage -l "$ITB" | grep -E "Data Size|Configuration|FIT description" | head -12
