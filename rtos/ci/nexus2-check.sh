#!/usr/bin/env bash
# Type-checks the full nexus2 station for armv7-rtems-eabihf (build-std) and writes a compact
# report of what still does not build on RTEMS: unique errors grouped by crate and file.
# usage: rtos/ci/nexus2-check.sh <toolchain prefix> <bsp prefix> <nexus source> <report>
set -uo pipefail
TOOLS=${1:?toolchain}; PREFIX=${2:?bsp prefix}; SRC=${3:?nexus source}; REPORT=${4:?report}
PKG=$PREFIX/lib/pkgconfig/arm-rtems6-xilinx_zynq_zedboard.pc
export PATH="$TOOLS/bin:$PATH"
export CC_armv7_rtems_eabihf=arm-rtems6-gcc CXX_armv7_rtems_eabihf=arm-rtems6-g++ AR_armv7_rtems_eabihf=arm-rtems6-ar
export CFLAGS_armv7_rtems_eabihf="$(PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig pkg-config --cflags "$PKG")"
export CXXFLAGS_armv7_rtems_eabihf="$CFLAGS_armv7_rtems_eabihf"
# std and the crates on the corrected RTEMS libc (nexus2 vendor/libc): a linked nightly whose
# rust-src workspace patches libc (rtos/ci/rust-toolchain.sh); the workspace [patch] covers
# the crates themselves
RUST_TOOLCHAIN=${RUST_TOOLCHAIN:-$("$(dirname "$0")/rust-toolchain.sh" nightly-2025-07-01 "$SRC/vendor/libc" nexus-rtems-libc)}
cd "$SRC"
# getrandom 0.3 has no RTEMS backend: nexus2 provides a custom one (bins/nexus2/src/rtems_entropy.rs)
export RUSTFLAGS="${RUSTFLAGS:-} --cfg getrandom_backend=\"custom\""
cargo +"$RUST_TOOLCHAIN" check -p nexus2 --release --target armv7-rtems-eabihf \
  -Zbuild-std=std,panic_abort --message-format short > "$REPORT.raw" 2>&1
rc=$?
{
  echo "nexus2 check for armv7-rtems-eabihf: exit $rc"
  echo "== errors per file"
  grep -E ': error(\[E[0-9]+\])?:' "$REPORT.raw" | cut -d: -f1 | sort | uniq -c | sort -rn | head -60
  echo "== distinct errors"
  grep -E 'error(\[E[0-9]+\])?:' "$REPORT.raw" | sed -E 's/^[^ ]+: //' | sort | uniq -c | sort -rn | head -80
  echo "== build script / link failures"
  grep -E 'failed to run custom build command|error: could not compile' "$REPORT.raw" | sort -u | head -30
} > "$REPORT"
# the report holds compiler messages about the private source: it goes to the draft release
# only; the public log gets the counts
echo "nexus2 check: exit $rc, $(grep -cE ': error(\[E[0-9]+\])?:' "$REPORT.raw" || true) errors (report in the draft release)"
exit 0
