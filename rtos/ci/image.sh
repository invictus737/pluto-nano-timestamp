#!/usr/bin/env bash
# Builds the M1/M2 application and packs it with the bitstream into rtos.itb / rtos.dfu.
# usage: rtos/ci/image.sh <toolchain prefix> <bsp prefix> <system_top.xsa> <out dir>
set -euo pipefail
TOOLS=${1:?toolchain}; PREFIX=${2:?bsp prefix}; XSA=${3:?xsa}; OUT=${4:?out}; HERE=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
make -C "$HERE/app" RTEMS_TOOLS="$TOOLS" RTEMS_PREFIX="$PREFIX" B="$OUT/app"
T=$(mktemp -d); unzip -q -o "$XSA" system_top.bit -d "$T"
cp "$HERE/image/rtos.its" "$OUT/app/nexus-rtos.bin" "$T/"
(cd "$T" && mkimage -f rtos.its "$OUT/rtos.itb" >/dev/null)
mkimage -l "$OUT/rtos.itb"
cp "$OUT/rtos.itb" "$OUT/rtos.dfu"
cp "$OUT/app/nexus-rtos.elf" "$OUT/app/nexus-rtos.map" "$OUT/"
(cd "$OUT" && sha256sum rtos.itb nexus-rtos.elf | tee SHA256SUMS)
