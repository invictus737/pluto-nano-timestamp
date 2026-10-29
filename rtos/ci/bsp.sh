#!/usr/bin/env bash
# RTEMS 6.2 kernel for the Pluto BSP and rtems-libbsd (FreeBSD 14 base) with the Pluto pieces.
# Incremental: the libbsd tree (sources + waf build directory) lives in $LIBBSD_WORK and is cached
# by CI. A restored tree gets its pristine copies of the patched files back, the overlay re-applied
# and only the changed objects rebuilt (waf compares contents). The kernel is built once per
# kernel-input hash (rtos/bsp/**, toolchain) and stays in the prefix.
# usage: rtos/ci/bsp.sh <toolchain prefix> <bsp prefix>
set -euo pipefail
TOOLS=${1:?toolchain prefix}; PREFIX=${2:?bsp prefix}; VER=6.2; HERE=$(cd "$(dirname "$0")/.." && pwd)
WORK=${LIBBSD_WORK:-$PWD/work-rtems}; BSP=arm/xilinx_zynq_zedboard
URL=https://ftp.rtems.org/pub/rtems/releases/6/$VER/sources
export PATH="$TOOLS/bin:$PATH"
mkdir -p "$WORK" "$PREFIX"
if [ ! -f "$PREFIX/.pluto-kernel" ]; then
  echo "kernel: full build"
  T=$(mktemp -d); (cd "$T" && curl -sfL "$URL/rtems-$VER.tar.xz" | tar xJ)
  cp "$HERE/bsp/config.ini" "$T/rtems-$VER/config.ini"
  (cd "$T/rtems-$VER" && ./waf configure --prefix="$PREFIX" | tail -3 && ./waf -j"$(nproc)" | tail -2 && ./waf install | tail -1)
  rm -rf "$T"; touch "$PREFIX/.pluto-kernel"
else
  echo "kernel: cached"
fi
LB=$WORK/rtems-libbsd-fb14-$VER
PATCHED="libbsd.py freebsd/sys/dev/usb/usb_msctest.c freebsd/sys/dev/usb/usb_msctest.h freebsd/sys/dev/usb/quirk/usb_quirk.c freebsd/sys/net80211/ieee80211_proto.c freebsd/sys/net80211/ieee80211_ioctl.c freebsd/sys/dev/rtwn/if_rtwn.c freebsd/sys/dev/usb/controller/ehci.c"
INI_SUM=$(sha256sum "$HERE/libbsd/pluto.ini" | cut -c1-16)
if [ ! -f "$LB/.pluto-configured" ]; then
  echo "libbsd: fresh tree"
  rm -rf "$LB"; (cd "$WORK" && curl -sfL "$URL/rtems-libbsd-fb14-$VER.tar.xz" | tar xJ)
  for f in $PATCHED; do mkdir -p "$LB/.pristine/$(dirname "$f")"; cp -p "$LB/$f" "$LB/.pristine/$f"; done
else
  echo "libbsd: cached tree, incremental build"
  # a file patched for the first time has no pristine copy yet: it is still unpatched
  for f in $PATCHED; do
    if [ -f "$LB/.pristine/$f" ]; then cp "$LB/.pristine/$f" "$LB/$f"
    else mkdir -p "$LB/.pristine/$(dirname "$f")"; cp -p "$LB/$f" "$LB/.pristine/$f"; fi
  done
fi
"$HERE/libbsd/aic8800/fetch-firmware.sh" "$WORK/aicwf_fw_data.c"
"$HERE/libbsd/apply.sh" "$LB" >/dev/null
cd "$LB"
if [ "$(cat .pluto-configured 2>/dev/null)" != "$INI_SUM" ]; then
  ./waf configure --prefix="$PREFIX" --rtems="$PREFIX" --rtems-tools="$TOOLS" --rtems-bsps=$BSP --buildset=buildset/pluto.ini | tail -3
  echo "$INI_SUM" > .pluto-configured
fi
S=$SECONDS; ./waf -j"$(nproc)" | tail -2; echo "libbsd build: $((SECONDS - S)) s"
./waf install | tail -1
install -D -m 644 "$WORK/aicwf_fw_data.c" "$PREFIX/share/aicwf/aicwf_fw_data.c"
ls -la "$PREFIX/arm-rtems6/xilinx_zynq_zedboard/lib/libbsd.a"
