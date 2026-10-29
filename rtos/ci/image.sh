#!/usr/bin/env bash
# Builds the application and packs it with the bitstream into rtos.itb (= rtos.dfu for dfu_ram).
# usage: rtos/ci/image.sh <toolchain prefix> <bsp prefix> <system_top.xsa> <out dir>
set -euo pipefail
TOOLS=${1:?toolchain}; PREFIX=${2:?bsp prefix}; XSA=${3:?xsa}; OUT=${4:?out}; HERE=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
RUST_LIB=${RUST_LIB:-}; [ -z "$RUST_LIB" ] || [ -f "$RUST_LIB" ] || { echo "no Rust library at $RUST_LIB, building without"; RUST_LIB=; }
# SSH server (M2e) when rtos/ci/wolfssh.sh installed the wolf libraries; secrets from the env
WOLF=${WOLF_PREFIX:-/opt/rtems/wolf}; [ -f "$WOLF/lib/libwolfssh.a" ] || WOLF=
# USB host stack (rtos/app/Makefile): libbsd (default) or cherry (rtos/cherryusb, fetched first)
USB_STACK=${USB_STACK:-libbsd}
app() { rm -rf "$OUT/app"; make -k -C "$HERE/app" RTEMS_TOOLS="$TOOLS" RTEMS_PREFIX="$PREFIX" B="$OUT/app" RUST_LIB="$1" WOLF_PREFIX="${2:-/nonexistent}" USB_STACK="$USB_STACK"; }
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
ELF=$OUT/app/nexus-rtos.elf
OBJCOPY=$TOOLS/bin/arm-rtems6-objcopy "$HERE/ci/pack.sh" "$ELF" "$XSA" "$OUT/rtos.itb"
cp "$ELF" "$OUT/app/nexus-rtos.map" "$OUT/"
(cd "$OUT" && sha256sum rtos.itb nexus-rtos.elf | tee SHA256SUMS)
