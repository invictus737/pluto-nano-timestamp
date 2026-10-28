#!/usr/bin/env bash
# Adds the Pluto pieces to an unpacked rtems-libbsd tree: the Zynq EHCI glue (nexus device,
# compiled for every BSP but guarded by LIBBSP_ARM_XILINX_ZYNQ_BSP_H) and the Pluto buildset.
# usage: rtos/libbsd/apply.sh <rtems-libbsd dir>
set -euo pipefail
LB=${1:?rtems-libbsd dir}; HERE=$(cd "$(dirname "$0")" && pwd)
cp "$HERE/ehci_zynq.c" "$LB/rtemsbsd/sys/dev/usb/controller/ehci_zynq.c"
cp "$HERE/pluto.ini" "$LB/buildset/pluto.ini"
grep -q "ehci_zynq.c" "$LB/libbsd.py" || python3 - "$LB/libbsd.py" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
a = "'sys/dev/usb/controller/ehci_mpc83xx.c',"
assert s.count(a) == 1, "anchor not unique in libbsd.py"
i = s.index(a); ind = s[s.rindex("\n", 0, i) + 1:i]
s = s.replace(a, a + "\n" + ind + "'sys/dev/usb/controller/ehci_zynq.c',")
open(p, "w").write(s)
PY
grep -n "ehci_zynq.c" "$LB/libbsd.py"
