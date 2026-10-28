#!/usr/bin/env bash
# Builds the probe as a static library for the RTEMS application (nightly + build-std).
# usage: rtos/rust-probe/build.sh <toolchain prefix> <bsp prefix> <out lib>
set -euo pipefail
TOOLS=${1:?toolchain}; PREFIX=${2:?bsp prefix}; OUT=${3:?out lib}; HERE=$(cd "$(dirname "$0")" && pwd)
PKG=$PREFIX/lib/pkgconfig/arm-rtems6-xilinx_zynq_zedboard.pc
export PATH="$TOOLS/bin:$PATH"
export CC_armv7_rtems_eabihf=arm-rtems6-gcc AR_armv7_rtems_eabihf=arm-rtems6-ar
export CFLAGS_armv7_rtems_eabihf="$(PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig pkg-config --cflags "$PKG")"
cd "$HERE"
cargo +"${RUST_TOOLCHAIN:-nightly}" build --release 2>&1 | tail -20
cp target/armv7-rtems-eabihf/release/libnexus_rust_probe.a "$OUT"
ls -la "$OUT"
