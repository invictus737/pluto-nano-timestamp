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
# aic8800_fdrv calls symbols exported by aic_load_fw: hand it that module's Module.symvers
SYMS=""
for sub in aic_load_fw aic8800_fdrv; do
  make -C "$FW/linux" ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- M="$WORK/aic8800d80/drivers/aic8800/$sub" \
    CONFIG_AIC_LOADFW_SUPPORT=m CONFIG_AIC8800_WLAN_SUPPORT=m CONFIG_PLATFORM_UBUNTU=n \
    ${SYMS:+KBUILD_EXTRA_SYMBOLS="$SYMS"} modules -j"$(nproc)"
  SYMS="$WORK/aic8800d80/drivers/aic8800/$sub/Module.symvers"
done
D=$OVL/lib/modules/$KREL/extra; mkdir -p "$D"
find "$WORK/aic8800d80/drivers/aic8800" -name '*.ko' -exec arm-linux-gnueabihf-strip --strip-debug {} \; -exec install -m 0644 {} "$D/" \;
mkdir -p "$OVL/lib/firmware/aic8800D80" && cp -a "$WORK/aic8800d80/fw/aic8800D80/." "$OVL/lib/firmware/aic8800D80/"
# AIC8800 adapters enumerate first as a USB stick (mass storage with the Windows driver CD):
# a69c:5721..572f, or 1111:1111 on "Pandora" clones. mdev ejects / mode-switches them, the adapter
# re-enumerates as a69c:8d80 (Wi-Fi) and the driver is loaded then. Same IDs as the driver's udev rules.
mkdir -p "$OVL/etc/usb_modeswitch.d" "$OVL/lib/mdev"
cp "$WORK/aic8800d80/usb_modeswitch/1111_1111" "$OVL/etc/usb_modeswitch.d/1111:1111"
cat > "$OVL/lib/mdev/aic8800.sh" <<'S'
#!/bin/sh
# mdev helper. $1 = disk: called for every sd* device; $1 = wifi: called for usb:va69cp8d80 interfaces.
usbdev() { d=$(readlink -f /sys/class/block/$MDEV/device 2>/dev/null); while [ -n "$d" ] && [ "$d" != / ]; do [ -f "$d/idVendor" ] && { echo "$d"; return; }; d=${d%/*}; done; }
case "$1" in
disk)
  [ "$ACTION" = add ] || exit 0
  u=$(usbdev); [ -n "$u" ] || exit 0
  vid=$(cat "$u/idVendor"); pid=$(cat "$u/idProduct")
  case "$vid:$pid" in
    a69c:572[1-7]|a69c:572[acf]) logger -t aic8800 "eject $MDEV ($vid:$pid, driver CD mode)"; eject /dev/$MDEV; exit 0 ;;
  esac
  exec /lib/mdev/automounter.sh ;;
wifi)
  [ "$ACTION" = add ] || exit 0
  lsmod | grep -q '^aic8800_fdrv' && exit 0
  logger -t aic8800 "AIC8800D80 Wi-Fi present, loading driver"
  insmod /lib/modules/$(uname -r)/extra/aic_load_fw.ko 2>/dev/null
  insmod /lib/modules/$(uname -r)/extra/aic8800_fdrv.ko 2>/dev/null
  # The card enumerates only after S96usbrole made the port a host, long after S40network ran
  # ifup -a: bring wlan0 up (wpa_supplicant + DHCP) once it exists, if S40network configured it.
  ( i=0; while [ ! -e /sys/class/net/wlan0 ] && [ $i -lt 30 ]; do sleep 1; i=$((i+1)); done
    if [ -e /sys/class/net/wlan0 ] && grep -q '^auto wlan0' /etc/network/interfaces; then
      ifup wlan0 > /dev/null 2>&1 && logger -t aic8800 "wlan0 up" || logger -t aic8800 "ifup wlan0 failed"
    fi ) < /dev/null > /dev/null 2>&1 & ;;
clone)
  [ "$ACTION" = add ] && usb_modeswitch -v 0x1111 -p 0x1111 -c /etc/usb_modeswitch.d/1111:1111 ;;
esac
exit 0
S
chmod +x "$OVL/lib/mdev/aic8800.sh"
# mdev.conf: the stock Pluto rules, with sd* routed through the AIC helper first (it chains to the automounter)
cat > "$OVL/etc/mdev.conf" <<'M'
$MODALIAS=usb:v1111p1111.*   root:root 660 @/lib/mdev/aic8800.sh clone
$MODALIAS=usb:va69cp8d80.*   root:root 660 @/lib/mdev/aic8800.sh wifi
sd[a-z]           root:root 660 */lib/mdev/aic8800.sh disk
sd[a-z][0-9]      root:root 660 */lib/mdev/automounter.sh
wlan[0-9]         root:root 660 */lib/mdev/ifupdown.sh
usb[0-9]          root:root 660 */lib/mdev/ifupdown.sh
eth[0-9]          root:root 660 */lib/mdev/ifupdown.sh
M
# adapter already plugged at boot: replay its uevents once mdev is running
mkdir -p "$OVL/etc/init.d"
cat > "$OVL/etc/init.d/S36aic8800" <<'S'
#!/bin/sh
case "$1" in start)
  for u in /sys/bus/usb/devices/*; do
    [ -f "$u/idVendor" ] || continue
    case "$(cat $u/idVendor):$(cat $u/idProduct)" in a69c:*|1111:1111) echo add > "$u/uevent"; for i in "$u"/*:*; do [ -f "$i/uevent" ] && echo add > "$i/uevent"; done ;; esac
  done
  for b in /sys/class/block/sd[a-z]; do [ -e "$b/uevent" ] && echo add > "$b/uevent"; done ;;
esac
exit 0
S
chmod +x "$OVL/etc/init.d/S36aic8800"
ls -la "$D" "$OVL/lib/firmware/aic8800D80" | head -20
