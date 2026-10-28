#!/usr/bin/env bash
# The libc crate std and nexus2 are built with (nexus2 vendor/libc, target_os = "rtems") against
# the installed RTEMS 6.2 + libbsd headers: every checked size, alignment, offset and constant is
# compiled by arm-rtems6-gcc with the BSP flags and by rustc for armv7-rtems-eabihf, and the two
# must agree. Also fails when rtos/app/abi_table.h (the `abi` shell command) is out of date.
# usage: rtos/ci/nexus2-abi.sh <toolchain prefix> <bsp prefix> <nexus source> <out dir>
#   RUST_TOOLCHAIN: toolchain with the patched std sources (rtos/ci/rust-toolchain.sh)
set -euo pipefail
TOOLS=${1:?toolchain}; PREFIX=${2:?bsp prefix}; SRC=${3:?nexus source}; OUT=${4:?out dir}
HERE=$(cd "$(dirname "$0")/.." && pwd)
PKG=$PREFIX/lib/pkgconfig/arm-rtems6-xilinx_zynq_zedboard.pc
export PATH="$TOOLS/bin:$PATH"
export ABI_CC=arm-rtems6-gcc
ABI_CFLAGS="$(PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig pkg-config --cflags "$PKG")"
export ABI_CFLAGS
export RUST_TOOLCHAIN=${RUST_TOOLCHAIN:-$("$HERE/ci/rust-toolchain.sh" nightly-2025-07-01 "$SRC/vendor/libc" nexus-rtems-libc)}
rc=0
"$SRC/scripts/rtems-abi/check.sh" "$OUT" || rc=1
if ! cmp -s "$OUT/abi_table.h" "$HERE/app/abi_table.h"; then
  echo "::error::rtos/app/abi_table.h differs from the nexus2 checks: copy $OUT/abi_table.h (abi-table.h in the draft release)"
  rc=1
fi
exit $rc
