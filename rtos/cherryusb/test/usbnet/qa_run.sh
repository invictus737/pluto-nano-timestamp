#!/usr/bin/env bash
# Independent QA tests of the USB Ethernet glue (qa_usbnet.c, qa_classes.c), same environment
# as run.sh (Linux, gcc, a fetched CherryUSB; macOS: qa_run-colima.sh). Exit status: the
# number of failing QA programs. usage: qa_run.sh [qa_usbnet test-name]
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SRC=${CHERRY_SRC:-$ROOT/CherryUSB}
AIC=${AICWF_DIR:-$ROOT/../libbsd/aic8800}
OUT=${OUT:-$(mktemp -d)}
CC=${CC:-gcc}
SAN=${SAN:-address}
CF=(-std=gnu11 -O1 -g -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function
    -Wno-missing-field-initializers -pthread
    -I"$ROOT/config" -I"$ROOT/net" -I"$SRC/common" -I"$SRC/core" -I"$SRC/class/hub" -I"$SRC/class/cdc"
    -I"$SRC/class/vendor/net" -I"$AIC" -I"$HERE")
[ "$SAN" != none ] && CF+=(-fsanitize="$SAN" -fno-omit-frame-pointer)
fails=0
$CC "${CF[@]}" "$ROOT/net/usbnet.c" "$HERE/qa_usbnet.c" -o "$OUT/qa_usbnet" || exit 99
$CC "${CF[@]}" -D'_Static_assert(a,b)=' "$ROOT/net/usbnet_classes.c" "$HERE/qa_classes.c" -o "$OUT/qa_classes" || exit 99
"$OUT/qa_classes" || fails=$((fails + 1))
"$OUT/qa_usbnet" "$@" || fails=$((fails + 1))
exit $fails
