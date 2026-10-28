#!/usr/bin/env bash
# The station image: the C side (RTEMS, libbsd, Wi-Fi, SSH, config) + nexus2 + the native Pluto
# SoapySDR driver, packed as rtos-nexus2.itb.
# usage: rtos/ci/image-nexus2.sh <toolchain> <bsp prefix> <sdr prefix> <nexus source> <xsa> <out dir>
set -euo pipefail
TOOLS=${1:?toolchain}; BSP=${2:?bsp}; SDR=${3:?sdr prefix}; SRC=${4:?nexus source}; XSA=${5:?xsa}; OUT=${6:?out}
HERE=$(cd "$(dirname "$0")/.." && pwd); mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
WOLF=${WOLF_PREFIX:-/opt/rtems/wolf}; WOLF_LIBS=
if [ -f "$WOLF/lib/libwolfssh.a" ]; then WOLF_LIBS="$WOLF/lib/libwolfssh.a $WOLF/lib/libwolfssl.a -ltelnetd"
else WOLF=/nonexistent; echo "::warning::station image WITHOUT the SSH server (no wolfSSH in ${WOLF_PREFIX:-/opt/rtems/wolf})"; fi
[ -f "$SDR/lib/libSoapyPlutoRtems.a" ] && [ -f "$SDR/lib/libSoapySDR.a" ] || { echo "no SoapySDR/Pluto driver libraries in $SDR"; exit 1; }
rm -rf "$OUT/n2app"
make -k -C "$HERE/app" RTEMS_TOOLS="$TOOLS" RTEMS_PREFIX="$BSP" B="$OUT/n2app" WOLF_PREFIX="$WOLF" nexus2lib
export NEXUS_RTEMS_SOAPY_LIB=$SDR/lib
export NEXUS_EXTRA_WHOLE=$SDR/lib/libSoapyPlutoRtems.a
export NEXUS_EXTRA_LIBS="$SDR/lib/libSoapySDR.a $WOLF_LIBS -ljffs2"
"$HERE/ci/nexus2-link.sh" "$TOOLS" "$BSP" "$SRC" "$OUT/n2app/libnexusrtos.a" -- "$OUT/nexus2.elf"
OBJCOPY=$TOOLS/bin/arm-rtems6-objcopy "$HERE/ci/pack.sh" "$OUT/nexus2.elf" "$XSA" "$OUT/rtos-nexus2.itb"
(cd "$OUT" && sha256sum rtos-nexus2.itb | tee -a SHA256SUMS)
