#!/usr/bin/env bash
# Native Pluto (AD9363A) SoapySDR driver for arm-rtems6: libSoapyPlutoRtems.a = the driver
# (rtos/sdr/pluto_rtems: SoapySDR device + static "plutosdr" registry, AXI DMAC streaming, framing,
# RTEMS platform layer for no-OS) + the ADI no-OS ad9361 driver subset from fetch-noos.sh.
#
# The image must link the archive with --whole-archive: nothing references the driver, it only
# registers itself through a static SoapySDR::Registry object (nexus2-link.sh:
# NEXUS_EXTRA_WHOLE=<prefix>/lib/libSoapyPlutoRtems.a, and libSoapySDR.a + -lstdc++ after it).
# Without whole-archive, -Wl,-u,soapy_pluto_rtems_registered keeps the driver object as well.
#
# usage: rtos/sdr/build-pluto-driver.sh <toolchain prefix> <bsp prefix> <SoapySDR prefix> \
#                                       <no-OS dir (fetch-noos.sh dest)> <install prefix>
set -euo pipefail
TOOLS=${1:?toolchain}; BSP=${2:?bsp prefix}; SOAPY=${3:?SoapySDR prefix}; NOOSD=${4:?no-OS dir}; OUT=${5:?install prefix}
HERE=$(cd "$(dirname "$0")" && pwd)
D=$HERE/pluto_rtems
N=$NOOSD/no-OS
PKG=$BSP/lib/pkgconfig/arm-rtems6-xilinx_zynq_zedboard.pc
export PATH="$TOOLS/bin:$PATH"
CFLAGS=$(PKG_CONFIG_PATH=$BSP/lib/pkgconfig pkg-config --cflags "$PKG")
[ -f "$N/drivers/rf-transceiver/ad9361/ad9361.c" ] || { echo "no-OS not found in $N (run fetch-noos.sh)"; exit 1; }
[ -f "$SOAPY/include/SoapySDR/Device.hpp" ] || { echo "SoapySDR headers not found in $SOAPY/include"; exit 1; }

INC=(-I"$D" -I"$N/include" -I"$N/drivers/rf-transceiver/ad9361" -I"$N/drivers/axi_core/axi_adc_core"
     -I"$N/drivers/axi_core/axi_dac_core")
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
mkdir -p "$W/o"

# ADI no-OS (third party: warnings off). app_config.h comes from pluto_rtems (first on the path).
NOOS_SRCS="drivers/rf-transceiver/ad9361/ad9361.c drivers/rf-transceiver/ad9361/ad9361_api.c
  drivers/rf-transceiver/ad9361/ad9361_conv.c drivers/rf-transceiver/ad9361/ad9361_util.c
  drivers/axi_core/axi_adc_core/axi_adc_core.c drivers/axi_core/axi_dac_core/axi_dac_core.c
  util/no_os_util.c util/no_os_alloc.c util/no_os_mutex.c drivers/api/no_os_spi.c drivers/api/no_os_gpio.c"
for f in $NOOS_SRCS; do
  b=$(basename "${f%.c}")
  arm-rtems6-gcc $CFLAGS -O2 -g -std=gnu11 -w "${INC[@]}" -c "$N/$f" -o "$W/o/noos_$b.o"
done

# Driver
for f in pluto_framing.c pluto_ring.c pluto_dma.c pluto_rt.c pluto_hw.c platform_spi.c platform_gpio.c platform_misc.c; do
  arm-rtems6-gcc $CFLAGS -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE "${INC[@]}" \
    -c "$D/$f" -o "$W/o/${f%.c}.o"
done
arm-rtems6-g++ $CFLAGS -O2 -g -std=gnu++14 -Wall -Wextra -Wno-unused-parameter \
  -I"$D" -I"$SOAPY/include" -c "$D/SoapyPlutoRtems.cpp" -o "$W/o/SoapyPlutoRtems.o"

mkdir -p "$OUT/lib" "$OUT/include"
rm -f "$OUT/lib/libSoapyPlutoRtems.a"
arm-rtems6-ar rcsD "$OUT/lib/libSoapyPlutoRtems.a" "$W"/o/*.o
cp "$D/pluto_rt.h" "$OUT/include/"
arm-rtems6-nm "$OUT/lib/libSoapyPlutoRtems.a" | grep -q " D soapy_pluto_rtems_registered" \
  && echo "registry object: present (link with --whole-archive or -Wl,-u,soapy_pluto_rtems_registered)"
echo "external RTEMS/SoapySDR symbols used:"
{ arm-rtems6-nm -u "$OUT/lib/libSoapyPlutoRtems.a" | awk '$1 == "U" {print $2}' | grep -E "^(rtems_|pthread_|SoapySDR_)" | sort -u | tr '\n' ' '; } || true; echo
arm-rtems6-size -t "$OUT/lib/libSoapyPlutoRtems.a" | tail -1
