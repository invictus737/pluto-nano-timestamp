#!/usr/bin/env bash
# AIC8800D80/D81 USB Wi-Fi driver, built IN the kernel tree as ordinary modules (no out-of-tree taint):
# the pinned community driver (github.com/shenmintao/aic8800d80, branch legacy-mcu1) plus
# patches/aic8800 is copied to drivers/net/wireless/aic8800 and hooked into Kconfig/Makefile; the
# kernel config (kernel-nano.config) selects it as modules. Its firmware goes to /lib/firmware of the
# generated overlay, where the kernel's request_firmware() finds it.
# usage: aic8800-intree.sh <linux tree> <generated rootfs overlay>   (before the kernel is configured)
set -euo pipefail
LINUX=$(cd "${1:?linux tree}" && pwd); OVL=$(mkdir -p "${2:?overlay}" && cd "$2" && pwd); HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$HERE/work-aic8800; mkdir -p "$WORK"
# legacy-mcu1 = the maintained branch for chip_id=7 chip_mcu_id=1 (this Nano's adapter, SDK V3 firmware
# that fits the MCU1 upload window); a9683a4 is its newest commit (2026-10-21)
AIC_REV=${AIC_REV:-a9683a456079131a0381ed75fb7a4f5514527b3c}
# Wi-Fi TX power ceiling written into the driver's userconfig (dBm, every rate, 2.4 and 5 GHz)
AIC_TXPWR_MAX=${AIC_TXPWR_MAX:-15}
if [ "$(git -C "$WORK/aic8800d80" rev-parse HEAD 2>/dev/null)" != "$AIC_REV" ]; then
  rm -rf "$WORK/aic8800d80" && git init -q "$WORK/aic8800d80"
  git -C "$WORK/aic8800d80" fetch -q --depth 1 https://github.com/shenmintao/aic8800d80.git "$AIC_REV"
  git -C "$WORK/aic8800d80" checkout -q FETCH_HEAD
fi
SRC=$WORK/src; rm -rf "$SRC" && cp -a "$WORK/aic8800d80" "$SRC"
for p in "$HERE"/patches/aic8800/*.patch; do git -C "$SRC" apply --whitespace=nowarn "$p" && echo "aic8800: applied $(basename "$p")"; done
D=$LINUX/drivers/net/wireless/aic8800
rm -rf "$D" && cp -a "$SRC/drivers/aic8800" "$D"
grep -q 'aic8800/Kconfig' "$LINUX/drivers/net/wireless/Kconfig" || \
  sed -i 's|^endif # WLAN|source "drivers/net/wireless/aic8800/Kconfig"\n\nendif # WLAN|' "$LINUX/drivers/net/wireless/Kconfig"
grep -q 'aic8800/' "$LINUX/drivers/net/wireless/Makefile" || \
  echo 'obj-$(CONFIG_AIC_WLAN_SUPPORT) += aic8800/' >> "$LINUX/drivers/net/wireless/Makefile"
grep -q 'aic8800/Kconfig' "$LINUX/drivers/net/wireless/Kconfig" || { echo "aic8800: Kconfig hook failed"; exit 1; }
# Firmware: the matched D80 set, under the names the driver requests, directly in /lib/firmware.
# Normal (station) mode uploads only these; the RF-test (lmacfw_rf, host_wb), IP-camera (_ipc),
# BLE-scan-wakeup and calibmode images belong to modes this image never enables.
mkdir -p "$OVL/lib/firmware"
for f in fw_patch_table_8800d80_u02.bin fw_adid_8800d80_u02.bin fw_patch_8800d80_u02.bin \
         fmacfw_8800d80_u02.bin aic_userconfig_8800d80.txt; do
  install -m 0644 "$SRC/fw/aic8800D80/$f" "$OVL/lib/firmware/$f"
done
UC=$OVL/lib/firmware/aic_userconfig_8800d80.txt
awk -v max="$AIC_TXPWR_MAX" -F= 'BEGIN { OFS = "=" }
  /^lvl_/ && $2 + 0 > max { $2 = max } { print }' "$UC" > "$UC.new" && mv "$UC.new" "$UC"
sed -i "1a # Pluto Nano: every lvl_* TX power entry capped at $AIC_TXPWR_MAX dBm (firmware/aic8800-intree.sh)" "$UC"
# the D80 legacy (SDK V3) parser has no txpwr_loss keys: it logs "invalid cmd: loss_enable/loss_value"
sed -i -e '/^loss_enable=/d' -e '/^loss_value=/d' -e '/^# txpwr_loss/d' "$UC"
! grep -q '^loss_' "$UC" || { echo "aic8800: loss keys left in the userconfig"; exit 1; }
grep -E '^lvl_' "$UC" | awk -F= -v max="$AIC_TXPWR_MAX" '$2 + 0 > max { bad = 1 } END { exit bad }' || { echo "aic8800: TX power cap failed"; exit 1; }
grep -E '^(enable|lvl_11b_11ag_1m_2g4|lvl_11a_6m_5g)=' "$UC"
ls -la "$OVL/lib/firmware" | grep 8800d80
