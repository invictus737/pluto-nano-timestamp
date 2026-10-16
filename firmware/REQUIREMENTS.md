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
4. **USB host mode** must stay selectable: the single OTG port is the gadget today (iiod over
   RNDIS/ECM); a USB adapter needs host mode with external power and a hub, or the second USB
   port if the board exposes one. The gadget remains the default.
5. Keep the vendor's U-Boot and environment (DFU recovery path), replace only `firmware.dfu`.
