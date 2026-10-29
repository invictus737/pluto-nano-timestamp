#!/usr/bin/env bash
# Host test of the USB Ethernet glue (net/usbnet.c) with fakes of the CherryUSB class drivers
# and of the libbsd ifnet (fake_usbnet.c), real pthreads, AddressSanitizer + LeakSanitizer.
# Needs a fetched CherryUSB (rtos/cherryusb/fetch-cherryusb.sh; CHERRY_SRC overrides), gcc and
# Linux (the class buffers' section attribute is ELF). On macOS run run-colima.sh.
# usage: run.sh [cycles]     (env: OUT=dir CC=gcc SAN=address|thread|none)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)          # rtos/cherryusb
SRC=${CHERRY_SRC:-$ROOT/CherryUSB}
AIC=${AICWF_DIR:-$ROOT/../libbsd/aic8800}
OUT=${OUT:-$(mktemp -d)}
CC=${CC:-gcc}
SAN=${SAN:-address}
CF=(-std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function
    -Wno-missing-field-initializers -pthread
    -I"$ROOT/config" -I"$ROOT/net" -I"$SRC/common" -I"$SRC/core" -I"$SRC/class/hub" -I"$SRC/class/cdc"
    -I"$SRC/class/vendor/net" -I"$AIC" -I"$HERE")
[ "$SAN" != none ] && CF+=(-fsanitize="$SAN" -fno-omit-frame-pointer)
set -x
$CC "${CF[@]}" "$ROOT/net/usbnet.c" "$HERE/fake_usbnet.c" "$HERE/test_usbnet.c" -o "$OUT/test_usbnet"
set +x
# TSan needs its fixed shadow layout: no ASLR (container kernels with high mmap bases)
if [ "$SAN" = thread ] && setarch "$(uname -m)" -R true 2>/dev/null; then
  exec setarch "$(uname -m)" -R "$OUT/test_usbnet" "$@"
fi
"$OUT/test_usbnet" "$@"
