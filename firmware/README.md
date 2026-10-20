Base: ADI `analogdevicesinc/plutosdr-fw` tag `v0.39` (hdl 065c8f1, linux 6.1 f3da30d, buildroot
2023.02.5 e783aad, u-boot-xlnx 90401ce), with pgreenland's RX/TX timestamping ported on top by
`fetch-fw.sh` (until v0.38 the base was pgreenland's `v0.38_timestamp` superproject):

- `patches/v0.39/hdl`: pgreenland's `system_bd.tcl` integration (hdl 1283650) resolved against the
  v0.39 block design. v0.39 gives both DMAs a plain `sync` port (SYNC_TRANSFER_START on both) and
  dropped SYNC from the `fifo_wr` interface: `cpack/packed_sync` feeds the RX timestamp packer
  explicitly, `util_wr_sync_mux` (timestamp block sync or TDD channel 1) drives `adc_dma/sync`, TDD
  channel 2 reaches `dac_dma/sync` through a second synchroniser (the DMA stream side runs on
  sys_cpu_clk here), and the TX timestamp unpacker is reset by `axi_ad9361/rst` only.
- `hdl-quantulum/`: pgreenland's IP repository (d701022) vendored, copied to the superproject root.
  Its private copy of the old `fifo_wr` interface (with SYNC) is removed and the SYNC port maps
  dropped, so ADI's interface definition is the only one; `refresh.tcl` re-checksums the cores.
- `patches/v0.39/linux`: `# CONFIG_GCC_PLUGINS is not set` (linux f763232); Winbond extended
  address register (0002, below).
- `patches/v0.39/buildroot`: `sdr_usb_gadget` @7339ab5, S23udc FunctionFS wiring, device_config
  product string (buildroot 79a8629, without htop).

ADI releases v0.39 for Vivado 2023.2. With 2022.2 (the only installer uploaded so far)
`build-hdl.sh` sets `ADI_IGNORE_VERSION_CHECK=1`; `firmware.yml` takes the version as an input.

Changes for the Nano, applied by `apply-nano.sh` (from `build.sh`, `build-hdl.sh`, `build-fw.sh`):

1. `hdl/projects/pluto/system_project.tcl`: part `xc7z010clg400-1` instead of `xc7z010clg225-1`.
2. `hdl/projects/pluto/system_constr.xdc`: replaced by `../board/nano/system_constr.xdc` (Nano balls,
   same port names as ADI's pluto project).
3. `linux/arch/arm/boot/dts/zynq-pluto-sdr*.dts`: Nano differences from `../board/nano/dts/nano.dtsi`
   (USB PHY reset GPIO 46, no ADM1177, LED on gpio0 15, model string).
4. FIT (`scripts/pluto.its`): unchanged layout, one bitstream shared by the Rev.A/B/C configurations.

## Flash layout (W25Q256, 32 MiB)

The Xilinx spi-nor path on the Zynq QSPI keeps 3-byte addresses above 16 MiB and switches banks
with the flash's extended address register (EAR). It only knew Spansion/Micron/Macronix/PMC: on the
Winbond the EAR read failed (`failed to read ear reg`) and its write went out without Write Enable,
so every access above 16 MiB wrapped onto the lower half. Patch 0002 adds Winbond (0xEF: RDEAR 0xC8,
WREN + WREAR 0xC5); `spi_nor_shutdown` returns the EAR to 0 before a reboot.

| offset | size | partition | |
|---|---|---|---|
| 0x0000000 | 1 MiB | qspi-fsbl-uboot | vendor FSBL + U-Boot |
| 0x0100000 | 128 KiB | qspi-uboot-env | |
| 0x0120000 | 896 KiB | qspi-nvmfs | JFFS2 `/mnt/jffs2` (stock) |
| 0x0200000 | 14 MiB | qspi-linux | `pluto.frm` (FIT), must stay below 16 MiB (`build-fw.sh` checks) |
| 0x1000000 | 1 MiB | qspi-bootcopy | read-only copy of qspi-fsbl-uboot: the BootROM starts here after a reset with EAR=1 |
| 0x1100000 | 15 MiB | qspi-data | UBIFS `/mnt/data` (S21data), applications (S97app) and their data |

S21data formats an empty or foreign qspi-data only when the EAR is readable and the partition does
not read back the lower bank; it warns when qspi-bootcopy differs from qspi-fsbl-uboot.

## USB adapters (OTG host)

Wi-Fi: AIC8800D80/D81 (out-of-tree driver, branch `legacy-mcu1`, `build-aic8800.sh`), Realtek
r8188eu/rtl8xxxu/rtl8192cu (in kernel, `rtlwifi` firmware); `wpa_supplicant` has nl80211 and wext
(r8188eu is wext only) and the ifup hook tries both. Ethernet: r8152 (RTL8153, RTL8156/8156B 2.5G,
`rtl_nic` firmware), ASIX, SMSC, CDC ECM/NCM, RNDIS. Bluetooth: kernel btusb + bluez
(bluetoothd, bluetoothctl); AIC combo adapters hand the HCI interface to btusb after the firmware
upload, 368b:8d81 with the `aic_zlp_quirk` module. `/lib/mdev/ifupdown.sh` brings any hot-plugged
`wlan*`/`eth*` up from `/etc/network/interfaces` (config.txt) and keeps DHCP retrying.

## Station role: slave (default) or master

`config.txt` [SYSTEM] `operating_mode = slave|master` (U-Boot environment `operating_mode`), or
`nexus-role slave|master` on the device; applied at once and at every boot, shown at login.

- **slave** (default): the Pluto is the SDR of a NexusBS running elsewhere, e.g. a Raspberry Pi.
  Both timestamp SDR gadgets run: USB FunctionFS (`sdr_usb_gadget`, `uri=usb:...,direct=1`) and
  UDP (`sdr_ip_gadget` 30432/30433, `uri=ip:<pluto>,direct=1,timestamp_every=...`), plus iiod.
  No application is started.
- **master**: NexusBS runs on the Pluto (S97app, `/mnt/data/<name>/app.conf`) and uses the radio
  directly; both SDR gadgets are stopped and the USB SDR function is left out of the composite.
  Switching rebuilds the USB composite, so a USB link drops for a few seconds.

In the master role the application opens the radio with `driver=plutosdr,uri=local:` plus
`local_timestamp=1` (NexusBS) or `direct=1`, and `timestamp_every=N` (soapyplutosdr-ts patch 0003,
`PlutoSDR_LocalTS`): the FPGA's timestamped DMA blocks are used in place, no gadget, no extra thread
or queue, waits are poll() on the DMA buffer (no CPU while idle, no clock syscall on the fast path).
One block per N samples with its 64-bit tick header, the gadget streams' semantics. Start/stop are
idempotent; a busy device (gadget still running), DMA errors and timeline restarts are logged
rate-limited and the DMA buffer is reopened. The stream mutex now blocks instead of spinning.

Buffers, two different limits:
- **DMA queue** `local_buffers_rx`/`local_buffers_tx`, default **16/16** (as deployed with the
  gadget; RX4 lost samples). A full TX queue is back-pressure: `writeStream` times out, no loss.
- **FPGA admission window: 8 blocks.** The DAC gate drops a header stamped more than
  16 x N/2 bus words = **8 x N samples** ahead of the DAC (and any late one), whatever the queue
  depth. The application's TX lead must stay under 8 blocks (4 ms at N = rate/2000, 8 ms at
  N = rate/1000), on every timestamped path: local, USB and IP gadget. The local TX streamer
  samples the gate's drop counter (0x800000B8) every 1000 blocks and logs any increase.

The tick unit of every timestamped stream is the requested sample rate (the AD9361 readback is
rounded, 599999 for 600000). Behaviour test against a libiio mmap model: `test/` in the patch.

## Tetra Amp Plus controller

The amplifier's RP2040 (pico-sdk USB CDC, VID 2e8a) appears as `/dev/tetra-amp` (mdev, other
CDC-ACM devices keep their ttyACM name). `tetra-amp <command>` sends one console command and prints
the reply (`tetra-amp status`, `tetra-amp tx on`, `tetra-amp help`), `tetra-amp watch` streams the
status, `tetra-amp flash <file.uf2>` puts the RP2040 into BOOTSEL (1200-baud touch) and copies the
file to its RPI-RP2 drive (automounted under /media).

## WireGuard

Kernel WireGuard + `wg`. Put a wg-quick style file in `/mnt/data/wireguard/wg0.conf` (or
`/mnt/jffs2/wireguard/`); S50wireguard brings it up with `wg-up` once NTP has set the clock (the
peer refuses handshakes stamped 1970 as replays) and a default route exists. IPv4 Address/MTU and
AllowedIPs routes are handled (0.0.0.0/0 as two /1 routes, endpoint kept on the old gateway);
DNS/PostUp and the like are ignored. `fw_setenv wireguard 0` disables it.

Output: `build/pluto.frm` and `build/pluto.dfu`. Flash with `dfu-util -a firmware.dfu -D build/pluto.dfu`
after the vendor backup. Host driver: `pgreenland/SoapyPlutoSDR` branch `sdr_gadget_timestamping`,
probe with `SoapySDRUtil --probe="driver=plutosdr,uri=ip:192.168.2.1,direct=1,timestamp_every=1500"`.

## Building on GitHub (no own hardware)

`.github/workflows/firmware.yml`, manual dispatch, two jobs:

1. **hdl** (ubuntu-22.04, ~45 GB after cleanup, 16 GB RAM): downloads the Vivado 2022.2 web
   installer from the private release `vivado-installer` of this repo, authenticates with the
   `AMD_USER`/`AMD_PASS` secrets (`xsetup -b AuthTokenGen`), installs **Vivado ML Standard with the
   Zynq-7000 devices only** (`firmware/vivado-install.sh`, no licence file needed for xc7z010), then
   runs `firmware/build-hdl.sh` (apply-nano + `make -C hdl/projects/pluto`). Artifact:
   `nano-system_top.xsa`.
2. **firmware**: clones plutosdr-fw with all submodules, drops the xsa into `build/`, assembles
   `pluto.frm`/`pluto.dfu` with the stock Makefile (no Vivado). Artifact: `pluto-nano-timestamp-firmware`.

Owner steps, once: create the AMD account secrets, download the web installer
(`Xilinx_Unified_2022.2_1014_8888_Lin64.bin`, about 270 MB, needs the AMD login on the download page)
and attach it to a release named `vivado-installer` (`gh release create vivado-installer <file>`).
Expect 1.5–2.5 h per run; the Vivado install is repeated every run (GitHub's cache is capped at 10 GB).
