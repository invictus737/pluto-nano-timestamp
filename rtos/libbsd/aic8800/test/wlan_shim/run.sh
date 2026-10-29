#!/usr/bin/env bash
# Host test of the station lifecycle across USB resets (shim/wlan_dev_shim.c on rtt_shim.c's
# pthread RT-Thread layer, fake WLAN device, ifnet and USB host), under AddressSanitizer.
# usage: run.sh   (Linux; on macOS run-colima.sh)   env: OUT=dir CC=gcc SAN=address|thread|none
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
A=$(cd "$HERE/../.." && pwd)   # rtos/libbsd/aic8800
R=$A/rtsmart
OUT=${OUT:-$(mktemp -d)}
CC=${CC:-gcc}
SAN=${SAN:-address}
CF=(-std=gnu11 -O1 -g -Wall -Wno-unused-function -Wno-format -pthread
    -I"$A/shim" -I"$R/aic8800" -I"$R/aic8800/port" -I"$R/wlan" -I"$R/wlan/offload" -I"$A")
[ "$SAN" != none ] && CF+=(-fsanitize="$SAN" -fno-omit-frame-pointer)
$CC "${CF[@]}" "$A/shim/wlan_dev_shim.c" "$A/shim/rtt_shim.c" "$HERE/test_wlan_shim.c" -o "$OUT/test_wlan_shim"
# the station's threads live forever: their RT-Thread objects are not leaks
printf 'leak:rt_thread_create\n' > "$OUT/lsan.supp"
# TSan needs its fixed shadow layout: no ASLR (container kernels with high mmap bases)
if [ "$SAN" = thread ] && setarch "$(uname -m)" -R true 2>/dev/null; then
  exec setarch "$(uname -m)" -R "$OUT/test_wlan_shim" "$@"
fi
LSAN_OPTIONS=suppressions="$OUT/lsan.supp" "$OUT/test_wlan_shim" "$@"
