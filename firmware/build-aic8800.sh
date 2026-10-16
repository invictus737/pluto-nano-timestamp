#!/usr/bin/env bash
# AIC8800D80 Wi-Fi driver (github.com/shenmintao/aic8800d80) built out of tree against the firmware's
# own kernel tree (after zImage), installed with its firmware into the rootfs overlay.
# usage: build-aic8800.sh <plutosdr-fw checkout> <rootfs-overlay dir>
set -euo pipefail
FW=$(cd "${1:?fw}" && pwd); OVL=$(mkdir -p "${2:?overlay}" && cd "$2" && pwd); HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$HERE/work-aic8800; mkdir -p "$WORK"
[ -d "$WORK/aic8800d80" ] || git clone -q --depth 1 https://github.com/shenmintao/aic8800d80.git "$WORK/aic8800d80"
# the Pluto flow builds only zImage; external modules need Module.symvers and scripts/module.lds
make -C "$FW/linux" ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- modules_prepare -j"$(nproc)" >/dev/null
make -C "$FW/linux" ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- modules -j"$(nproc)" >/dev/null
KREL=$(make -s -C "$FW/linux" ARCH=arm kernelrelease)
for sub in aic_load_fw aic8800_fdrv; do
  make -C "$FW/linux" ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- M="$WORK/aic8800d80/drivers/aic8800/$sub" \
    CONFIG_AIC_LOADFW_SUPPORT=m CONFIG_AIC8800_WLAN_SUPPORT=m CONFIG_PLATFORM_UBUNTU=n modules -j"$(nproc)"
done
D=$OVL/lib/modules/$KREL/extra; mkdir -p "$D"
find "$WORK/aic8800d80/drivers/aic8800" -name '*.ko' -exec arm-linux-gnueabihf-strip --strip-debug {} \; -exec install -m 0644 {} "$D/" \;
mkdir -p "$OVL/lib/firmware/aic8800D80" && cp -a "$WORK/aic8800d80/fw/aic8800D80/." "$OVL/lib/firmware/aic8800D80/"
# load on demand when the adapter appears (mdev), aic_load_fw first
mkdir -p "$OVL/etc/init.d"
cat > "$OVL/etc/init.d/S36aic8800" <<'S'
#!/bin/sh
# AIC8800D80 USB Wi-Fi: load the driver only if such an adapter is present (VID a69c).
case "$1" in start)
  if grep -qi '^PRODUCT=a69c/' /sys/bus/usb/devices/*/uevent 2>/dev/null; then
    insmod /lib/modules/$(uname -r)/extra/aic_load_fw.ko 2>/dev/null
    insmod /lib/modules/$(uname -r)/extra/aic8800_fdrv.ko 2>/dev/null
  fi;;
esac
exit 0
S
chmod +x "$OVL/etc/init.d/S36aic8800"
ls -la "$D" "$OVL/lib/firmware/aic8800D80" | head -20
