#!/usr/bin/env bash
# QA tests of the ehci_nxp driver (qa_ehci_nxp.c): the base suite's harness plus CPU-overhead
# counters (register accesses, critical-section hold time via --wrap), idle/steady-state wakeups,
# interrupt intervals, page crossing / ZLP, deferred-cancel livelock, error storms and a long soak.
# usage: qa_run.sh [test-name-substring ...]  (env: OUT=dir CC=gcc SEED=n QA_SOAK=transfers)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SRC=${CHERRY_SRC:-$ROOT/CherryUSB}
OUT=${OUT:-$(mktemp -d)}
CC=${CC:-gcc}
[ "$(uname -m)" = aarch64 ] || { echo "qa_run.sh: needs arm64 Linux" >&2; exit 2; }
CF=(-std=gnu11 -O1 -g -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
    -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -no-pie -fno-pie -fno-strict-aliasing
    -I"$ROOT/config" -I"$ROOT/port/ehci_nxp" -I"$ROOT/port" -I"$SRC/common" -I"$SRC/core"
    -I"$SRC/class/hub" -I"$SRC/port/ehci" -I"$HERE")
$CC "${CF[@]}" -include "$HERE/qa_regs.h" -c "$ROOT/port/ehci_nxp/usb_hc_ehci_nxp.c" -o "$OUT/drv.o"
for f in sim_hc sim_mmio sim_dev sim_glue qa_ehci_nxp; do
  $CC "${CF[@]}" -c "$HERE/$f.c" -o "$OUT/$f.o"
done
$CC "${CF[@]}" -Wl,--wrap=usb_osal_enter_critical_section -Wl,--wrap=usb_osal_leave_critical_section -Wl,--wrap=usb_osal_msleep \
    "$OUT"/drv.o "$OUT"/sim_hc.o "$OUT"/sim_mmio.o "$OUT"/sim_dev.o "$OUT"/sim_glue.o \
    "$OUT"/qa_ehci_nxp.o -o "$OUT/qa_ehci_nxp"
"$OUT/qa_ehci_nxp" "$@"
