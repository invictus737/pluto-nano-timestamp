#!/usr/bin/env bash
# Assemble pluto.frm / pluto.dfu from a prebuilt Nano system_top.xsa (no Vivado needed here).
# usage: build-fw.sh <plutosdr-fw checkout> <system_top.xsa>
set -euo pipefail
FW=${1:?plutosdr-fw checkout}; XSA=${2:?system_top.xsa}; HERE=$(cd "$(dirname "$0")" && pwd)
"$HERE/apply-nano.sh" "$FW"
mkdir -p "$FW/build" && cp "$XSA" "$FW/build/system_top.xsa"
# kernel: merge the Nano fragment into the Pluto defconfig
cat "$HERE/kernel-nano.config" >> "$FW/linux/arch/arm/configs/zynq_pluto_defconfig"
# buildroot: external tree (aic8800d80) + lean fragment merged into the Pluto defconfig
cat "$HERE/br-external/configs/pluto_nano_fragment.config" >> "$FW/buildroot/configs/zynq_pluto_defconfig"
export BR2_EXTERNAL="$HERE/br-external"
# cached across runs by .github/actions/fw-caches
cat >> "$FW/buildroot/configs/zynq_pluto_defconfig" <<BR
BR2_DL_DIR="$HOME/br-dl"
BR2_CCACHE=y
BR2_CCACHE_DIR="$HOME/.buildroot-ccache"
BR2_CCACHE_USE_BASEDIR=y
BR
# Kernel and U-Boot keep ADI's compiler (Linaro GCC 7.3 arm-linux-gnueabihf). plutosdr-fw used to take
# it from Buildroot's external toolchain; the rootfs is musl now, so provide it separately on PATH.
LINARO=gcc-linaro-7.3.1-2018.05-x86_64_arm-linux-gnueabihf
if [ ! -x "$HERE/work-linaro/$LINARO/bin/arm-linux-gnueabihf-gcc" ]; then
  mkdir -p "$HERE/work-linaro"
  curl -sL "https://sources.buildroot.net/toolchain-external-linaro-arm/$LINARO.tar.xz" | tar xJ -C "$HERE/work-linaro"
fi
export PATH="$HERE/work-linaro/$LINARO/bin:$PATH"
arm-linux-gnueabihf-gcc --version | head -1
cd "$FW" && export TARGET=pluto VIVADO_SETTINGS=/nonexistent
make build/zImage 2>&1 | tail -5
"$HERE/build-aic8800.sh" "$FW" "$HERE/br-external/board/nano/rootfs-overlay"
make build/pluto.frm 2>&1 | tail -30
ls -la build/pluto.frm; sha256sum build/pluto.frm
