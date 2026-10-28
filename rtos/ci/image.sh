#!/usr/bin/env bash
# Builds the application and packs it with the bitstream into rtos.itb (= rtos.dfu for dfu_ram).
# usage: rtos/ci/image.sh <toolchain prefix> <bsp prefix> <system_top.xsa> <out dir>
set -euo pipefail
TOOLS=${1:?toolchain}; PREFIX=${2:?bsp prefix}; XSA=${3:?xsa}; OUT=${4:?out}; HERE=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
RUST_LIB=${RUST_LIB:-}; [ -z "$RUST_LIB" ] || [ -f "$RUST_LIB" ] || { echo "no Rust library at $RUST_LIB, building without"; RUST_LIB=; }
if ! make -C "$HERE/app" RTEMS_TOOLS="$TOOLS" RTEMS_PREFIX="$PREFIX" B="$OUT/app" RUST_LIB="$RUST_LIB"; then
  [ -n "$RUST_LIB" ] || exit 1
  echo "::warning::link with the Rust library failed; image built without it"
  rm -rf "$OUT/app"; make -C "$HERE/app" RTEMS_TOOLS="$TOOLS" RTEMS_PREFIX="$PREFIX" B="$OUT/app"
fi
ELF=$OUT/app/nexus-rtos.elf; BIN=$OUT/app/nexus-rtos.bin
# Entry must be the lowest load address of the binary, and the binary must stay compact
# (an initialised section in the high non-cacheable region would blow it up to ~500 MB).
ENTRY=$(readelf -h "$ELF" | awk '/Entry point/ {print $4}')
LOWEST=$(readelf -lW "$ELF" | awk '$1=="LOAD" && $6!="0x000000" {print $4}' | sort | head -1)
SIZE=$(stat -c %s "$BIN")
echo "entry $ENTRY lowest LMA $LOWEST binary $SIZE bytes"
[ $((ENTRY)) -eq $((LOWEST)) ] || { echo "entry is not the lowest load address"; readelf -lW "$ELF"; exit 1; }
[ "$SIZE" -lt $((48 * 1024 * 1024)) ] || { echo "binary too large"; readelf -lW "$ELF"; exit 1; }
T=$(mktemp -d); unzip -q -o "$XSA" system_top.bit -d "$T"
dtc -I dts -O dtb -o "$T/stub.dtb" "$HERE/image/stub.dts"
sed "s/@ENTRY@/$ENTRY/g" "$HERE/image/rtos.its.in" > "$T/rtos.its"
cp "$BIN" "$T/"
(cd "$T" && mkimage -f rtos.its "$OUT/rtos.itb" >/dev/null)
mkimage -l "$OUT/rtos.itb"
cp "$ELF" "$OUT/app/nexus-rtos.map" "$OUT/"
(cd "$OUT" && sha256sum rtos.itb nexus-rtos.elf | tee SHA256SUMS)
