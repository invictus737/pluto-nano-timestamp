# Firmware requirements (station owner, 2026-10-16)

1. **Timestamps**: pgreenland sample timestamping (RX block timestamps, timed TX), the reason this
   firmware exists. Verified by `SoapySDRUtil --probe ... Timestamps: YES` with the
   `sdr_gadget_timestamping` host driver.
2. **Lean image.** The QSPI firmware partition is 30 MB; the vendor FIT is 12.3 MB (kernel 4.6 MB,
   ramdisk 6.6 MB, bitstream 1 MB). Keep the rootfs to what a radio needs: iiod, the USB gadget,
   dropbear, busybox, libiio, libad9361. No GNU Radio, no Python, no maia web stack, no DATV.
   Target: FIT ≤ 16 MB so drivers and an optional ARMv7 `nexus2` binary (about 8-15 MB stripped)
   still fit, with room for a future update slot.
3. **USB network drivers, built in the firmware** (the device has no compiler, DKMS is not an
   option). Kernel modules: `cdc_ether`, `rndis_host`, `r8152`, `ax88179_178a`, `asix`, `cdc_ncm`
   for USB Ethernet adapters; `aic8800d80` (community driver, github.com/shenmintao/aic8800d80,
   builds for `ARCH=arm`) plus its firmware blobs for the AIC8800D80 Wi-Fi adapter. Built as a
   buildroot external kernel-module package against the firmware's own kernel; modules are loaded
   on demand so they cost flash, not RAM.
4. **USB OTG host mode compiled in and selectable at boot (first-rank requirement).** Target
   setup: a small *powered* USB hub on the Pluto's USB-C port, carrying a Wi-Fi card (AIC8800D80)
   or a USB Ethernet adapter, with the hub's power also feeding the Pluto through that USB-C port
   (back-powering the host port). In that mode the station is reached over Wi-Fi/Ethernet, not over
   the USB gadget. Consequences:
   - kernel: the v0.38 `zynq_pluto_defconfig` already has `USB_OTG`, `USB_CHIPIDEA_HOST` + `UDC`,
     `USB_EHCI_HCD`, `USB_USBNET`, `RNDIS_HOST`, `RTL8152`, `SMSC95XX`, `DM9601`, `CFG80211`/`MAC80211`
     and `MODULES`. To add as modules: `USB_NET_CDCETHER`, `USB_NET_AX8817X`, `USB_NET_AX88179_178A`,
     `USB_NET_CDC_NCM`; out of tree: `aic8800d80` (`aic_load_fw`, `aic8800_fdrv`) + firmware blobs;
   - device tree: `usb0` stays `dr_mode = "otg"`; the role is chosen by a U-Boot variable
     (`usb_mode = peripheral|host`, default `peripheral` so iiod over USB keeps working out of the
     box) applied as a DT fixup at boot; a runtime switch through the ChipIdea role file is kept
     for tests;
   - rootfs: keep the stock Pluto mechanism, which is already the simplest possible: ADI's
     firmware ships `wpa_supplicant`, `wpa_cli`, `iw` and reads **`[WLAN] ssid_wlan / pwd_wlan /
     ipaddr_wlan`** from `config.txt` on the mass-storage drive (or `fw_setenv ssid_wlan ...`),
     then brings `wlan0` up itself on a USB dongle. No new tooling: the user writes SSID and
     password in `config.txt`, ejects, done. What changes: add the `aic8800d80` driver and its
     firmware so the AIC dongle is one of the supported adapters, and drop the stock Ralink/Realtek
     blobs (`BR2_PACKAGE_LINUX_FIRMWARE_RALINK_*`, `RTL_8*`) that only cost flash. USB Ethernet
     gets its address by `udhcpc` the same way the stock image handles `eth0`.
   - electrical: the Nano's USB-C is the only power input; back-powering it from a hub while the
     Zynq acts as host must be checked on the board (VBUS path, no OTG ID/CC conflict) before relying
     on it. A hub that does **not** back-power needs a separate 5 V feed to the Pluto.
5. Keep the vendor's U-Boot and environment (DFU recovery path), replace only `firmware.dfu`.

6. **Both CPU cores, stock clock.** `maxcpus=2` (vendor environment boots with 1; `S05cpus` fixes it
   once, override with `fw_setenv maxcpus 1`). CPU stays at 667 MHz (BogoMIPS 333.33 per core is the
   global-timer rate, CPU/2), DDR untouched: tezuka documents marginal DDR on Nano boards, so no
   DDR increase. A CPU-only overclock FSBL (`boot.dfu`) is a separate, opt-in image after a
   `memtester` run; a bad FSBL disables DFU recovery (JTAG only).
