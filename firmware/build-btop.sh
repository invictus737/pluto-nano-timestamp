#!/usr/bin/env bash
# btop for the Pluto: static ARMv7 (Cortex-A9, hard float) binary built with a Bootlin musl toolchain.
# Buildroot 2023.02 in plutosdr-fw has no btop package and its Linaro GCC 7.3 cannot build
# btop's C++20, so it is built here and dropped into the rootfs overlay.
# usage: build-btop.sh <rootfs-overlay dir>
set -euo pipefail
OVL=${1:?overlay dir}; HERE=$(cd "$(dirname "$0")" && pwd); WORK=${WORK:-$HERE/work-btop}
TC=armv7-eabihf--musl--stable-2024.05-1
BTOP=v1.4.0
mkdir -p "$WORK" && cd "$WORK"
[ -d "$TC" ] || curl -sL "https://toolchains.bootlin.com/downloads/releases/toolchains/armv7-eabihf/tarballs/$TC.tar.xz" | tar xJ
[ -d btop ] || git clone -q --depth 1 -b "$BTOP" https://github.com/aristocratos/btop.git
CXX=$PWD/$TC/bin/arm-linux-g++
STRIP=$PWD/$TC/bin/arm-linux-strip
make -C btop -j"$(nproc)" STATIC=true GPU_SUPPORT=false CXX="$CXX" ARCH=arm \
  ADDFLAGS="-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -Os" >/dev/null
"$STRIP" btop/bin/btop
install -D -m 0755 btop/bin/btop "$OVL/usr/bin/btop"
ls -la "$OVL/usr/bin/btop"; file "$OVL/usr/bin/btop" 2>/dev/null || true
