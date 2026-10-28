#!/usr/bin/env bash
# SoapySDR 0.8.1 core library (C++ API + C API) as a static library for arm-rtems6, without
# loadable modules (Modules_rtems.cpp). The native Pluto driver registers itself statically.
# usage: rtos/sdr/build-soapysdr.sh <toolchain prefix> <bsp prefix> <install prefix>
set -euo pipefail
TOOLS=${1:?toolchain}; BSP=${2:?bsp prefix}; OUT=${3:?install prefix}; HERE=$(cd "$(dirname "$0")" && pwd)
VER=0.8.1; SHA=a508083875ed75d1090c24f88abef9895ad65f0f1b54e96d74094478f0c400e6
PKG=$BSP/lib/pkgconfig/arm-rtems6-xilinx_zynq_zedboard.pc
export PATH="$TOOLS/bin:$PATH"
CFLAGS=$(PKG_CONFIG_PATH=$BSP/lib/pkgconfig pkg-config --cflags "$PKG")
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
curl -sfL -o "$W/s.tar.gz" "https://github.com/pothosware/SoapySDR/archive/refs/tags/soapy-sdr-$VER.tar.gz"
echo "sha256 $(sha256sum "$W/s.tar.gz" | cut -c1-64)"
[ "${SOAPY_SHA_CHECK:-1}" = 0 ] || [ "$(sha256sum "$W/s.tar.gz" | cut -c1-64)" = "$SHA" ] || { echo "SoapySDR tarball hash mismatch"; exit 1; }
tar xzf "$W/s.tar.gz" -C "$W"; S=$W/SoapySDR-soapy-sdr-$VER
sed -e "s/@SOAPY_SDR_VERSION@/$VER-nexus-rtems/" "$S/lib/Version.in.cpp" > "$W/Version.cpp"
SRCS="Device.cpp Factory.cpp Registry.cpp Types.cpp NullDevice.cpp Logger.cpp Errors.cpp Formats.cpp
      ConverterRegistry.cpp DefaultConverters.cpp TypesC.cpp ModulesC.cpp VersionC.cpp DeviceC.cpp
      FactoryC.cpp LoggerC.cpp TimeC.cpp ErrorsC.cpp FormatsC.cpp ConvertersC.cpp"
mkdir -p "$W/o"
for f in $SRCS; do
  arm-rtems6-g++ $CFLAGS -O2 -std=c++14 -DSOAPY_SDR_DLL=0 -I"$S/include" -I"$S/lib" -c "$S/lib/$f" -o "$W/o/${f%.cpp}.o"
done
arm-rtems6-g++ $CFLAGS -O2 -std=c++14 -I"$S/include" -c "$W/Version.cpp" -o "$W/o/Version.o"
arm-rtems6-g++ $CFLAGS -O2 -std=c++14 -I"$S/include" -c "$HERE/Modules_rtems.cpp" -o "$W/o/Modules.o"
mkdir -p "$OUT/lib" "$OUT/include"
rm -f "$OUT/lib/libSoapySDR.a"; arm-rtems6-ar rcsD "$OUT/lib/libSoapySDR.a" "$W"/o/*.o
cp -R "$S/include/SoapySDR" "$OUT/include/"
arm-rtems6-nm "$OUT/lib/libSoapySDR.a" | grep -c " T SoapySDRDevice_" | sed 's/^/C API functions: /'
