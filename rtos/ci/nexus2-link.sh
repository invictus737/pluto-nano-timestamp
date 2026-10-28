#!/usr/bin/env bash
# Links the full nexus2 station into the RTEMS image: the C side (RTEMS configuration, libbsd,
# drivers, SSH, config) is archived as libnexusrtos.a and pulled in whole; cargo builds the nexus2
# binary for armv7-rtems-eabihf and links everything with arm-rtems6-gcc. RTEMS' Init task then
# runs nexus2's main() in its own thread (rtos/app: NEXUS_RUST_MAIN).
# usage: rtos/ci/nexus2-link.sh <toolchain> <bsp prefix> <nexus source> <libnexusrtos.a> <extra lib dir...> -- <out elf>
set -euo pipefail
TOOLS=${1:?toolchain}; BSP=${2:?bsp}; SRC=${3:?nexus source}; APPLIB=${4:?libnexusrtos.a}; shift 4
LIBDIRS=(); while [ "$#" -gt 0 ] && [ "$1" != "--" ]; do LIBDIRS+=("$1"); shift; done; shift; OUT=${1:?out elf}
PKG=$BSP/lib/pkgconfig/arm-rtems6-xilinx_zynq_zedboard.pc
export PATH="$TOOLS/bin:$PATH"
CFLAGS=$(PKG_CONFIG_PATH=$BSP/lib/pkgconfig pkg-config --cflags "$PKG")
LDLIBS=$(PKG_CONFIG_PATH=$BSP/lib/pkgconfig pkg-config --libs "$PKG")
export CC_armv7_rtems_eabihf=arm-rtems6-gcc CXX_armv7_rtems_eabihf=arm-rtems6-g++ AR_armv7_rtems_eabihf=arm-rtems6-ar
export CFLAGS_armv7_rtems_eabihf="$CFLAGS" CXXFLAGS_armv7_rtems_eabihf="$CFLAGS"
LINK="-C linker=arm-rtems6-gcc"
for a in $CFLAGS $LDLIBS; do LINK="$LINK -C link-arg=$a"; done
for d in "${LIBDIRS[@]}"; do LINK="$LINK -C link-arg=-L$d"; done
# The C side is not referenced from Rust (RTEMS calls Init): pull every object in.
LINK="$LINK -C link-arg=-Wl,--whole-archive -C link-arg=$APPLIB -C link-arg=-Wl,--no-whole-archive"
for l in ${NEXUS_EXTRA_WHOLE:-}; do LINK="$LINK -C link-arg=-Wl,--whole-archive -C link-arg=$l -C link-arg=-Wl,--no-whole-archive"; done
LINK="$LINK -C link-arg=-Wl,--start-group"
for l in ${NEXUS_EXTRA_LIBS:-} -lbsd -lstdc++ -lm -lz; do LINK="$LINK -C link-arg=$l"; done
LINK="$LINK -C link-arg=-Wl,--end-group -C link-arg=-Wl,-Map,${OUT%.elf}.map"
export RUSTFLAGS="--cfg getrandom_backend=\"custom\" $LINK"
export CARGO_PROFILE_RELEASE_PANIC=abort
cd "$SRC"
cargo +"${RUST_TOOLCHAIN:-nightly-2025-07-01}" build -p nexus2 --release --target armv7-rtems-eabihf \
  -Zbuild-std=std,panic_abort 2>&1 | tail -40
cp target/armv7-rtems-eabihf/release/nexus2 "$OUT"
arm-rtems6-size "$OUT"
