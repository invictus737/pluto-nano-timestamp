#!/usr/bin/env bash
# Host test of the ehci_nxp CherryUSB host controller driver (port/ehci_nxp) against an EHCI 1.0
# controller model (ChipIdea/Zynq USB0 registers) and an AIC8800-like device (Wi-Fi bulk +
# Bluetooth interrupt/bulk/EP0 HCI). The driver source is compiled unchanged; its register
# macros are routed to the model (sim_regs.h), any other access to the register page is trapped.
#
# Needs: a fetched CherryUSB (rtos/cherryusb/fetch-cherryusb.sh), gcc, an arm64 Linux.
# Descriptors hold 32-bit addresses: the binary is linked non-PIE (driver pools below 4 GiB) and
# test buffers come from a fixed low arena (0x20000000). On macOS run run-colima.sh.
#
# usage: run.sh [test-name-substring ...]     (env: OUT=dir CC=gcc SAN=1 SEED=n)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SRC=${CHERRY_SRC:-$ROOT/CherryUSB}
OUT=${OUT:-$(mktemp -d)}
CC=${CC:-gcc}
[ "$(uname -m)" = aarch64 ] || { echo "run.sh: needs arm64 Linux (register trap decodes AArch64)" >&2; exit 2; }
CF=(-std=gnu11 -O1 -g -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
    -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -no-pie -fno-pie -fno-strict-aliasing
    -I"$ROOT/config" -I"$ROOT/port/ehci_nxp" -I"$ROOT/port" -I"$SRC/common" -I"$SRC/core"
    -I"$SRC/class/hub" -I"$SRC/port/ehci" -I"$HERE")
[ "${SAN:-0}" = 1 ] && CF+=(-fsanitize=undefined -fno-sanitize-recover=undefined)
set -x
$CC "${CF[@]}" -include "$HERE/sim_regs.h" -c "$ROOT/port/ehci_nxp/usb_hc_ehci_nxp.c" -o "$OUT/drv.o"
for f in sim_hc sim_mmio sim_dev sim_glue test_ehci_nxp; do
  $CC "${CF[@]}" -c "$HERE/$f.c" -o "$OUT/$f.o"
done
$CC "${CF[@]}" "$OUT"/drv.o "$OUT"/sim_hc.o "$OUT"/sim_mmio.o "$OUT"/sim_dev.o "$OUT"/sim_glue.o \
    "$OUT"/test_ehci_nxp.o -o "$OUT/test_ehci_nxp"
set +x
"$OUT/test_ehci_nxp" "$@"
