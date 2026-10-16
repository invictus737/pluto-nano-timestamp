# Build recipe (target: Nano bitstream with timestamping)

Base: `pgreenland/plutosdr-fw` branch `v0.38_timestamp` (submodules: ADI `plutosdr-hdl` hdl_2018_r1,
`plutosdr-hdl-quantulum` with `util_cpack2_timestamp`, `util_upack2_timestamp`, `util_wr_sync_mux`,
`plutosdr-linux` 2018_R1, buildroot, u-boot-xlnx pluto).

Changes for the Nano, applied by `build.sh` on a Vivado 2022.2 host:

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
