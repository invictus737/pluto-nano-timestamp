#!/usr/bin/env bash
# Kernel modules into the generated rootfs overlay the standard way: modules_install (stripped) runs
# depmod, which writes modules.dep and modules.alias; mdev's $MODALIAS rule then loads a driver with
# modprobe when its device appears. Run after the kernel (zImage) is built.
# usage: build-modules.sh <linux tree> <generated rootfs overlay>
set -euo pipefail
LINUX=$(cd "${1:?linux tree}" && pwd); OVL=$(mkdir -p "${2:?overlay}" && cd "$2" && pwd)
K="make -C $LINUX ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-"
$K modules -j"$(nproc)"
KREL=$($K -s kernelrelease)
rm -rf "$OVL/lib/modules/$KREL"
$K INSTALL_MOD_PATH="$OVL" INSTALL_MOD_STRIP=1 DEPMOD=/sbin/depmod modules_install > /dev/null
rm -f "$OVL/lib/modules/$KREL/build" "$OVL/lib/modules/$KREL/source"
# busybox modprobe reads the text files (modules.dep, modules.alias, modules.symbols, modules.builtin);
# the binary indexes and modules.builtin.modinfo are for kmod only
rm -f "$OVL/lib/modules/$KREL"/modules.*.bin "$OVL/lib/modules/$KREL/modules.builtin.modinfo"
for f in modules.dep modules.alias; do [ -s "$OVL/lib/modules/$KREL/$f" ] || { echo "no $f"; exit 1; }; done
for m in aic_load_fw aic8800_fdrv; do grep -q "/$m.ko:" "$OVL/lib/modules/$KREL/modules.dep" || { echo "module $m missing"; exit 1; }; done
grep -c '' "$OVL/lib/modules/$KREL/modules.alias" | sed 's/^/modules.alias lines: /'
find "$OVL/lib/modules/$KREL" -name '*.ko' -exec ls -la {} \;
