#!/usr/bin/env bash
# RTEMS 6.2 kernel for the Pluto BSP and rtems-libbsd (FreeBSD 14 base) with the Zynq EHCI glue.
# usage: rtos/ci/bsp.sh <toolchain prefix> <bsp prefix>
set -euo pipefail
TOOLS=${1:?toolchain prefix}; PREFIX=${2:?bsp prefix}; VER=6.2; WORK=${WORK:-$PWD/work-rtems}; HERE=$(cd "$(dirname "$0")/.." && pwd)
BSP=arm/xilinx_zynq_zedboard
export PATH="$TOOLS/bin:$PATH"
mkdir -p "$WORK" && cd "$WORK"
rm -rf rtems-$VER rtems-libbsd-fb14-$VER
curl -sfL https://ftp.rtems.org/pub/rtems/releases/6/$VER/sources/rtems-$VER.tar.xz | tar xJ
curl -sfL https://ftp.rtems.org/pub/rtems/releases/6/$VER/sources/rtems-libbsd-fb14-$VER.tar.xz | tar xJ
cd rtems-$VER
cp "$HERE/bsp/config.ini" config.ini
./waf configure --prefix="$PREFIX" | tail -5
./waf -j"$(nproc)" | tail -3
./waf install | tail -2
cd ../rtems-libbsd-fb14-$VER
"$HERE/libbsd/apply.sh" "$PWD"
./waf configure --prefix="$PREFIX" --rtems="$PREFIX" --rtems-tools="$TOOLS" --rtems-bsps=$BSP --buildset=buildset/pluto.ini | tail -5
./waf -j"$(nproc)" | tail -3
./waf install | tail -2
ls -la "$PREFIX/arm-rtems6/xilinx_zynq_zedboard/lib/libbsd.a" "$PREFIX/lib/pkgconfig/"
