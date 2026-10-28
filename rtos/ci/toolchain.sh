#!/usr/bin/env bash
# RTEMS 6.2 ARM toolchain (gcc, binutils, newlib, gdb) from the release source builder.
# usage: rtos/ci/toolchain.sh <prefix>
set -euo pipefail
PREFIX=${1:?prefix}; VER=6.2; WORK=${WORK:-$PWD/work-rtems}
mkdir -p "$WORK" "$PREFIX" && cd "$WORK"
[ -d rtems-source-builder-$VER ] || curl -sfL https://ftp.rtems.org/pub/rtems/releases/6/$VER/sources/rtems-source-builder-$VER.tar.xz | tar xJ
cd rtems-source-builder-$VER/rtems
../source-builder/sb-check
../source-builder/sb-set-builder --prefix="$PREFIX" --jobs="$(nproc)" 6/rtems-arm 2>&1 | tail -40 || {
  find . -name 'rsb-report-*.txt' -exec tail -80 {} \; ; exit 1; }
"$PREFIX/bin/arm-rtems6-gcc" --version | head -1
