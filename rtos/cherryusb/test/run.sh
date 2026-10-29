#!/usr/bin/env bash
# Host simulation of the patched CherryUSB EHCI port (IAA cycles, watchdog, quarantine, kill,
# barriers, bulk IN streams).
# The driver is compiled unchanged except that USBSTS/USBCMD stores become calls into a
# register model (write-1-to-clear status, doorbell that software cannot clear), so the test
# can answer, delay, drop or lose IAA. Needs a fetched CherryUSB (fetch-cherryusb.sh), gcc and
# python3; descriptors hold 32-bit addresses, so the binary is linked non-PIE (globals below
# 4 GiB): run it on Linux (on macOS: inside any amd64/arm64 Linux container with gcc).
# usage: rtos/cherryusb/test/run.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
SRC=${CHERRY_SRC:-$ROOT/CherryUSB}
OUT=${OUT:-$(mktemp -d)}
python3 - "$SRC/port/ehci/usb_hc_ehci.c" "$OUT/ehci_sim.c" <<'PY'
import re, sys
s = open(sys.argv[1]).read()
s = s.replace('(EHCI_HCOR->usbcmd = (EHCI_HCOR->usbcmd & ~EHCI_USBCMD_IAAD) | (bits))',
              'sim_usbcmd_write((EHCI_HCOR->usbcmd & ~EHCI_USBCMD_IAAD) | (bits))')
for op in ('|', '&'):
    s = re.sub(r'EHCI_HCOR->(usbsts|usbcmd)\s*\%s=\s*([^;]+);' % op,
               lambda m: 'sim_%s_write(EHCI_HCOR->%s %s (%s));' % (m.group(1), m.group(1), op, m.group(2)), s)
s = re.sub(r'EHCI_HCOR->(usbsts|usbcmd)\s*=(?!=)\s*([^;]+);',
           lambda m: 'sim_%s_write(%s);' % (m.group(1), m.group(2)), s)
assert 'EHCI_HCOR->usbsts =' not in s and 'EHCI_HCOR->usbcmd =' not in s
open(sys.argv[2], 'w').write(s)
PY
${CC:-gcc} -std=gnu11 -O1 -g -Wall -Wno-unused-function -Wno-unused-variable \
  -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -no-pie -fno-pie \
  -I"$OUT" -I"$ROOT/config" -I"$ROOT/port" -I"$SRC/common" -I"$SRC/core" \
  -I"$SRC/class/hub" -I"$SRC/port/ehci" \
  "$HERE/ehci_iaa_test.c" -o "$OUT/ehci_iaa_test"
"$OUT/ehci_iaa_test"
