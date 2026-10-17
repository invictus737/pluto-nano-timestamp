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
- `patches/v0.39/linux`: `# CONFIG_GCC_PLUGINS is not set` (linux f763232).
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
