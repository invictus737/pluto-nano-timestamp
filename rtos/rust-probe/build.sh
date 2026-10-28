#!/usr/bin/env bash
# Builds the probe as a static library for the RTEMS application (nightly + build-std).
# armv7-rtems-eabihf is tier 3: std breaks on some nightlies (libc struct changes), so the
# toolchains in RUST_TOOLCHAINS are tried in order and the first that builds is used.
# usage: rtos/rust-probe/build.sh <toolchain prefix> <bsp prefix> <out lib>
set -uo pipefail
TOOLS=${1:?toolchain}; PREFIX=${2:?bsp prefix}; OUT=${3:?out lib}; HERE=$(cd "$(dirname "$0")" && pwd)
PKG=$PREFIX/lib/pkgconfig/arm-rtems6-xilinx_zynq_zedboard.pc
export PATH="$TOOLS/bin:$PATH"
export CC_armv7_rtems_eabihf=arm-rtems6-gcc AR_armv7_rtems_eabihf=arm-rtems6-ar
export CFLAGS_armv7_rtems_eabihf="$(PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig pkg-config --cflags "$PKG")"
cd "$HERE"
# nightly-2025-07-01 is the newest that builds std for this target (2026-10-28: later ones fail on
# libc stat fields, dirfd and _SC_HOST_NAME_MAX).
for tc in ${RUST_TOOLCHAINS:-nightly-2025-07-01 nightly-2025-04-01 nightly}; do
  rustup toolchain install "$tc" --profile minimal --component rust-src >/dev/null 2>&1 || { echo "== $tc: not installable"; continue; }
  echo "== $tc: $(rustc +"$tc" -V)"
  if cargo +"$tc" build --release > "build-$tc.log" 2>&1; then
    cp target/armv7-rtems-eabihf/release/libnexus_rust_probe.a "$OUT"
    echo "$tc" > "$OUT.toolchain"
    ls -la "$OUT"; exit 0
  fi
  grep -E "^error|^ *--> " "build-$tc.log" | head -24
  rm -rf target
done
echo "no toolchain built std for armv7-rtems-eabihf"; exit 1
