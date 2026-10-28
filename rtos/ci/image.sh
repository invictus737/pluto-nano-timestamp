#!/usr/bin/env bash
# Builds the application and packs it with the bitstream into rtos.itb (= rtos.dfu for dfu_ram).
# usage: rtos/ci/image.sh <toolchain prefix> <bsp prefix> <system_top.xsa> <out dir>
set -euo pipefail
TOOLS=${1:?toolchain}; PREFIX=${2:?bsp prefix}; XSA=${3:?xsa}; OUT=${4:?out}; HERE=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
RUST_LIB=${RUST_LIB:-}; [ -z "$RUST_LIB" ] || [ -f "$RUST_LIB" ] || { echo "no Rust library at $RUST_LIB, building without"; RUST_LIB=; }
# SSH server (M2e) when rtos/ci/wolfssh.sh installed the wolf libraries; secrets from the env
WOLF=${WOLF_PREFIX:-/opt/rtems/wolf}; [ -f "$WOLF/lib/libwolfssh.a" ] || WOLF=
app() { rm -rf "$OUT/app"; make -C "$HERE/app" RTEMS_TOOLS="$TOOLS" RTEMS_PREFIX="$PREFIX" B="$OUT/app" RUST_LIB="$1" WOLF_PREFIX="${2:-/nonexistent}"; }
if ! app "$RUST_LIB" "$WOLF"; then
  if [ -n "$RUST_LIB" ] && app "" "$WOLF"; then
    echo "::warning::link with the Rust library failed; image built without it"
  elif [ -n "$WOLF" ] && app "$RUST_LIB" ""; then
    echo "::warning::SSH server build failed; image built without it"
  elif [ -n "$RUST_LIB" ] && [ -n "$WOLF" ] && app "" ""; then
    echo "::warning::Rust library and SSH server failed; image built without both"
  else
    exit 1
  fi
fi
ELF=$OUT/app/nexus-rtos.elf; BIN=$OUT/app/nexus-rtos.bin
# Load at the lowest load address (the vector table), enter at the ELF entry (_start); the
# binary must stay compact (an initialised section in the high non-cacheable region would
# blow it up to ~500 MB).
ENTRY=$(readelf -h "$ELF" | awk '/Entry point/ {print $4}')
LOAD=$(readelf -lW "$ELF" | awk '$1=="LOAD" && $5!="0x00000" {print $3}' | sort | head -1)
SIZE=$(stat -c %s "$BIN")
echo "load $LOAD entry $ENTRY binary $SIZE bytes"
[ $((ENTRY)) -ge $((LOAD)) ] && [ $((ENTRY)) -lt $((LOAD + 0x1000)) ] || { echo "entry not at the start of the image"; readelf -lW "$ELF"; exit 1; }
[ "$SIZE" -lt $((48 * 1024 * 1024)) ] || { echo "binary too large"; readelf -lW "$ELF"; exit 1; }
T=$(mktemp -d); unzip -q -o "$XSA" system_top.bit -d "$T"
dtc -I dts -O dtb -o "$T/stub.dtb" "$HERE/image/stub.dts"
sed -e "s/@LOAD@/$LOAD/g" -e "s/@ENTRY@/$ENTRY/g" "$HERE/image/rtos.its.in" > "$T/rtos.its"
cp "$BIN" "$T/"
(cd "$T" && mkimage -f rtos.its "$OUT/rtos.itb" >/dev/null)
mkimage -l "$OUT/rtos.itb"
cp "$ELF" "$OUT/app/nexus-rtos.map" "$OUT/"
(cd "$OUT" && sha256sum rtos.itb nexus-rtos.elf | tee SHA256SUMS)
