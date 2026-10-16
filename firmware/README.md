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
