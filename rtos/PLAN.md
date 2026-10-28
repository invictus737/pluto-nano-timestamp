# Nexus-BS 2.0 RTOS on the Pluto Nano (branch `Nexus-BS-2.0_RTOS`)

Goal: a complete Pluto image without Linux running the **full Nexus-BS 2.0 station**
(`bins/nexus2`: TETRA, DMR, P25, analog FM, networks, dashboard), not the Nexus-Light subset.
RTEMS 6.2 (hard real-time, SMP on both Cortex-A9 cores) runs the AD9363 radio and nexus2 in one
statically linked image. Network over USB (Pluto as USB host): USB-Ethernet dongles, USB Wi-Fi
(Realtek rtwn / Ralink from libbsd, and the user's AICSemi AIC8800 card with Wi-Fi + BT,
including the automatic storage-mode switch). Boot to RF in ~2-3 s.

Everything is built on GitHub Actions. Nothing here replaces the Linux firmware until a RAM
boot has been proven on the board.

## Why RTEMS and not FreeRTOS

The TETRA stack is Rust with `std` (threads, sockets, time). Rust has an official std target
for RTEMS (`armv7-rtems-eabihf`, tier 3, tested on `xilinx_zynq_a9_qemu`), none for FreeRTOS
on Cortex-A. RTEMS also brings the FreeBSD USB host stack and USB-Ethernet drivers via
libbsd. Same class of RTOS (static image, priority scheduling, no MMU processes).

## Hardware facts used

- xc7z010clg400, 512 MB DDR3, 32 MB QSPI, console UART1 (MIO 12..13, CH340 port).
- USB0 ChipIdea controller at 0xE0002000 (EHCI caps at +0x100), ULPI PHY, PHY reset MIO 46,
  GIC IRQ 53. Host mode needs a powered hub (as with the Linux `usb_mode host`).
- Boot chain stays: BootROM -> vendor FSBL -> Tezuka U-Boot 2025.01 (DFU, QSPI env).
- PL: the v0.39 timestamp bitstream (same XSA as the `v0.39-std` firmware).

## Image format

`rtos.itb` (FIT): `fpga` = bitstream (.bin), `kernel` = RTEMS binary (os = rtems, load/entry
0x00100000). U-Boot: `bootm ${fit_load_address}` loads the PL, copies the kernel and jumps.
RAM-boot first through `run dfu_ram`; flashing is a separate, explicit step.

## Milestones

| # | Deliverable | Acceptance (on the board) |
|---|---|---|
| M0 | CI: XSA from the v0.39-std run, RTEMS 6.2 toolchain + BSP + libbsd (cached), `rtos.itb` | artifact builds |
| M1 | Boot: console banner, both cores up, LED, uptime; PL loaded by U-Boot | RAM boot via DFU, banner on UART1 |
| M2 | USB host EHCI (Zynq glue in libbsd) + USB-Ethernet (axe/axge/ure incl. the Dell 2.5G RTL8156/8156B, cdce ECM/NCM), DHCP, HTTP status, shell on UART1 | ping + `curl /status` |
| M2b | USB Wi-Fi from libbsd: rtwn (RTL8188EU/8192CU/8812AU/8821AU), net80211, wpa_supplicant | WPA2 association, DHCP |
| M2c | AIC8800: storage-mode eject driver (SCSI eject via usb_msc_eject), fullmac Wi-Fi driver ported from aic8800_fdrv to net80211 (model: if_rsu), firmware in the image | D80 + DC sticks associate, DHCP |
| M2d | AIC8800 BT: HCI over USB (+ BT patch download), BTstack host, A2DP source like the Linux BT Player | A2DP to headphones |
| M2e | SSH: wolfSSH server (public-key auth, keys in the data partition) giving the RTEMS shell + nexus commands, SFTP/SCP for files and updates | `ssh` + `scp` to the Pluto |
| M3 | Rust std on `armv7-rtems-eabihf` (build-std): threads, sockets, time | Rust thread + UDP echo on board |
| M4 | AD9363 (ADI no-OS ad9361 driver) + AXI DMAC RX/TX with timestamps; `rf-front` backend `pluto-native` implementing RxIo/TxIo/DuplexIo instead of SoapySDR | RX samples flowing, TX on dummy load |
| M5 | Full `nexus2` on RTEMS: SoapySDR replaced by M4, `reqwest` (tokio/mio) gated or replaced by std-only HTTP, `getrandom`/`ring` RTEMS backend, `ctrlc` gated; config + calibration files on a QSPI file system | all four modes on air, dashboard, Brew/BrandMeister/P25/svxlink networks |
| M5b | Station roles kept as in the Linux firmware, same U-Boot env keys (`operating_mode` master/slave/light, `usb_mode` peripheral/auto/host, `xo_correction`): **master** = full nexus2 on the radio, **light** = nexus2 light mode, **slave** = SDR for an external NexusBS: iiod-compatible control (ad9361-phy/cf-ad9361 attributes) + sdr-ip-gadget timestamped streaming so the host's SoapyPlutoSDR (`direct=1,timestamp_every=...`) works unchanged; over IP (USB-Ethernet/Wi-Fi) first, then USB device mode (ChipIdea UDC: CDC-NCM + the existing gadget layout) | each role selected by env and proven: master/light on air, slave probed by SoapySDRUtil from a Pi |
| M6 | QSPI layout: two image slots A/B (whole RTEMS+nexus2 image, update written to the inactive slot over SFTP/dashboard, hash check, switch, automatic fallback) + JFFS2 data partition (nexus2.toml, calibration, SSH keys, logs); bank 0 restored before any reset; Linux kept for recovery | cold boot to RF in < 3 s, update + rollback proven |

## Decisions

- 2026-10-28 (user): licensing is not a constraint for now. The AIC8800 Wi-Fi/BT driver is ported
  from the GPL-2.0 Linux driver and SSH uses wolfSSH/wolfSSL; the licence combination is to be
  revisited before any distribution.

## Board state (2026-10-28)

- QSPI firmware slot 0x200000 holds rtos.itb f02f71bf (CI run 40732765838, 6533A1 bytes), env
  fit_config=config-2, fit_size=6533A1; U-Boot, env layout, jffs2 (0x120000) and UBI data
  (0x1A00000) untouched. Reset to RTEMS shell 5.3 s (2 s of it the U-Boot menu delay).
- Linux restore: backup in Nexus-BS-2.0/.local/pluto-backup-20261028-rtos (README.txt): run dfu_sf,
  dfu-util -a firmware.dfu -D firmware.bin, setenv fit_config config@1; setenv fit_size BA12D3; saveenv.
- After an RTEMS soft reset the ULPI PHY keeps host/VBUS state and U-Boot DFU is not seen by the
  host: run `gpio clear 46; gpio set 46` before dfu_sf/dfu_ram (or power-cycle). To fix in the glue.
- CPU at stock 666.67 MHz (cpu_clock=standard FSBL since 2026-10-28).

## Risks

- Rust std on RTEMS is tier 3 and "not fully tested".
- The nexus2 source (Nexus-BS-2.0, no remote) must reach the CI: private repo or release asset, to decide with the user.
- AIC8800 has no FreeBSD/RTEMS driver: the fullmac port is the largest new code block.
- libbsd EHCI on Zynq is new code (ChipIdea, same core as QorIQ/i.MX glue).
- U-Boot must leave CPU1 parked for RTEMS SMP start.
- Clock options (UART ref, PERIPHCLK) must match the Nano PS7 settings; read from the XSA.
