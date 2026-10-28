#!/usr/bin/env bash
# Adds the Pluto pieces to an unpacked rtems-libbsd tree (idempotent): the Zynq EHCI glue,
# the AIC8800 storage-mode switch and USB/ifnet attachment (aicwf), usb_msc_eject_cdb()
# and the Pluto buildset.
# usage: rtos/libbsd/apply.sh <rtems-libbsd dir>
set -euo pipefail
LB=${1:?rtems-libbsd dir}; HERE=$(cd "$(dirname "$0")" && pwd)
cp "$HERE/ehci_zynq.c" "$LB/rtemsbsd/sys/dev/usb/controller/ehci_zynq.c"
cp "$HERE/aic_modeswitch.c" "$LB/rtemsbsd/sys/dev/usb/aic_modeswitch.c"
mkdir -p "$LB/rtemsbsd/sys/dev/usb/wlan"
cp "$HERE/aic8800/if_aicwf_usb.c" "$HERE/aic8800/aicwf_usbif.h" "$LB/rtemsbsd/sys/dev/usb/wlan/"
cp "$HERE/pluto.ini" "$LB/buildset/pluto.ini"
python3 "$HERE/patch_libbsd.py" "$LB"
grep -n "ehci_zynq.c\|aic_modeswitch.c\|if_aicwf_usb.c" "$LB/libbsd.py"
