#!/usr/bin/env bash
# AIC8800D80 Wi-Fi driver (github.com/shenmintao/aic8800d80) built out of tree against the firmware's
# own kernel tree (after zImage), installed with its firmware into the rootfs overlay.
# usage: build-aic8800.sh <plutosdr-fw checkout> <rootfs-overlay dir>
set -euo pipefail
FW=$(cd "${1:?fw}" && pwd); OVL=$(mkdir -p "${2:?overlay}" && cd "$2" && pwd); HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$HERE/work-aic8800; mkdir -p "$WORK"
# pinned: branch legacy-mcu1, for chip_id=7 chip_mcu_id=1 adapters (the Nano's 368b:8d81 reports
# that): matched firmware, Bluetooth through the kernel's standard btusb. Builds against kernel 6.1.
AIC_REV=${AIC_REV:-e074b7ea98654e3e535e44b3959e4b9613f6e5f4}
if [ "$(git -C "$WORK/aic8800d80" rev-parse HEAD 2>/dev/null)" != "$AIC_REV" ]; then
  rm -rf "$WORK/aic8800d80" && git init -q "$WORK/aic8800d80"
  git -C "$WORK/aic8800d80" fetch -q --depth 1 https://github.com/shenmintao/aic8800d80.git "$AIC_REV"
  git -C "$WORK/aic8800d80" checkout -q FETCH_HEAD
fi
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
# 368b:8d81 Bluetooth needs zero-length packets on ACL bulk OUT: kprobe companion of btusb
make -C "$FW/linux" ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- M="$WORK/aic8800d80/drivers/aic8800/aic_zlp_quirk" modules -j"$(nproc)"
for m in aic_load_fw aic8800_fdrv aic_zlp_quirk; do
  [ -f "$WORK/aic8800d80/drivers/aic8800/$m/$m.ko" ] || { echo "missing $m.ko"; exit 1; }
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
# mdev helper. $1 = disk: called for every sd* device; $1 = wifi: called for AIC8800 USB interfaces
# (a69c:88xx/8dxx and 368b:8dxx, the Wi-Fi/Bluetooth personality after the mass-storage eject).
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
  # one uevent per USB interface: only the first loads the drivers
  mkdir /var/run/aic8800.lock 2>/dev/null || exit 0
  trap 'rmdir /var/run/aic8800.lock' EXIT
  lsmod | grep -q '^aic8800_fdrv' && exit 0
  K=/lib/modules/$(uname -r)/extra
  logger -t aic8800 "AIC8800 present ($MODALIAS), loading driver"
  # the loader defaults to /vendor/etc/firmware; this image keeps the blobs under /lib/firmware.
  # Its probe uploads the firmware (Wi-Fi and Bluetooth) before insmod returns.
  insmod $K/aic_load_fw.ko aic_fw_path=/lib/firmware/aic8800D80 2>/dev/null
  # power save off (station duty, latency), driver log at errors only
  insmod $K/aic8800_fdrv.ko ps_on=0 aicwf_dbg_level=1 2>/dev/null
  # Bluetooth: btusb (built in) claimed the HCI interface at enumeration, before the firmware
  # upload; rebind it so it initialises the controller now. 368b:8d81 also needs the ZLP quirk.
  for i in /sys/bus/usb/devices/*:*; do
    [ "$(cat $i/bInterfaceClass 2>/dev/null)" = e0 ] || continue
    u=$(readlink -f "$i/.."); case "$(cat $u/idVendor)" in a69c|368b) ;; *) continue ;; esac
    [ "$(cat $u/idVendor):$(cat $u/idProduct)" = 368b:8d81 ] && insmod $K/aic_zlp_quirk.ko 2>/dev/null
    n=${i##*/}
    [ -e /sys/bus/usb/drivers/btusb/$n ] && echo "$n" > /sys/bus/usb/drivers/btusb/unbind
    echo "$n" > /sys/bus/usb/drivers/btusb/bind 2>/dev/null && logger -t aic8800 "Bluetooth $n bound to btusb"
  done
  # wlan0 normally comes up through the net uevent (/lib/mdev/ifupdown.sh); repeat it once the
  # interface exists in case that event raced S40network writing /etc/network/interfaces
  ( i=0; while [ ! -e /sys/class/net/wlan0 ] && [ $i -lt 30 ]; do sleep 1; i=$((i+1)); done
    [ -e /sys/class/net/wlan0 ] && ACTION=add MDEV=wlan0 /lib/mdev/ifupdown.sh ) < /dev/null > /dev/null 2>&1 & ;;
clone)
  [ "$ACTION" = add ] && usb_modeswitch -v 0x1111 -p 0x1111 -c /etc/usb_modeswitch.d/1111:1111 ;;
esac
exit 0
S
chmod +x "$OVL/lib/mdev/aic8800.sh"
# mdev.conf: the stock Pluto rules, with sd* routed through the AIC helper first (it chains to the automounter)
cat > "$OVL/etc/mdev.conf" <<'M'
$MODALIAS=usb:v1111p1111.*   root:root 660 @/lib/mdev/aic8800.sh clone
$MODALIAS=usb:v([Aa]69[Cc]|368[Bb])p8[0-9A-Fa-f]{3}d.*   root:root 660 @/lib/mdev/aic8800.sh wifi
sd[a-z]           root:root 660 */lib/mdev/aic8800.sh disk
sd[a-z][0-9]      root:root 660 */lib/mdev/automounter.sh
wlan[0-9]         root:root 660 */lib/mdev/ifupdown.sh
usb[0-9]          root:root 660 */lib/mdev/ifupdown.sh
eth[0-9]          root:root 660 */lib/mdev/ifupdown.sh
ttyACM[0-9]+      root:root 660 */lib/mdev/tetra-amp.sh
M
# adapter already plugged at boot: replay its uevents once mdev is running
mkdir -p "$OVL/etc/init.d"
cat > "$OVL/etc/init.d/S36aic8800" <<'S'
#!/bin/sh
case "$1" in start)
  for u in /sys/bus/usb/devices/*; do
    [ -f "$u/idVendor" ] || continue
    case "$(cat $u/idVendor):$(cat $u/idProduct)" in a69c:*|368b:*|1111:1111) echo add > "$u/uevent"; for i in "$u"/*:*; do [ -f "$i/uevent" ] && echo add > "$i/uevent"; done ;; esac
  done
  for b in /sys/class/block/sd[a-z]; do [ -e "$b/uevent" ] && echo add > "$b/uevent"; done ;;
esac
exit 0
S
chmod +x "$OVL/etc/init.d/S36aic8800"
ls -la "$D" "$OVL/lib/firmware/aic8800D80" | head -20
