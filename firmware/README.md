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
WREN + WREAR 0xC5); `spi_nor_shutdown` returns the EAR to 0 before a reboot. U-Boot (vendor and
Tezuka 2025.01) addresses the whole flash correctly (verified: `sf read`/`crc32` of blocks at 0x0,
0x100000, 0xFF0000, 0x1000000 and 0x1100000 equal the Linux mtd reads).

The **stock** HamGeek/ADI kernel lacks that patch and declares qspi-linux as 30 MiB: its update.sh
`dd`s pluto.frm to mtdblock3, so anything past 0x1000000 lands on 0x0 (FSBL/U-Boot). A pluto.frm
installed from stock firmware must therefore be <= 14 MiB **including** its md5 trailer
(`build-fw.sh` checks it, with a 13.5 MiB target).

| offset | size | partition | |
|---|---|---|---|
| 0x0000000 | 1 MiB | qspi-fsbl-uboot | FSBL + U-Boot (boot.frm; apps.frm may replace the FSBL) |
| 0x0100000 | 128 KiB | qspi-uboot-env | U-Boot environment (`uboot/uboot-env-nexus.txt`) |
| 0x0120000 | 896 KiB | qspi-nvmfs | JFFS2 `/mnt/jffs2` (stock) |
| 0x0200000 | 14 MiB | qspi-linux | `pluto.frm` (FIT) |
| 0x1000000 | 1 MiB | qspi-bootcopy | copy of qspi-fsbl-uboot: the BootROM starts here after a reset with EAR=1; kept equal by apps.frm |
| 0x1100000 | 9 MiB | qspi-apps | application slot (apps.frm), read only at run time |
| 0x1A00000 | 6 MiB | qspi-data | UBIFS `/mnt/data` (S21data): configuration and application data |

S21data formats an empty or foreign qspi-data only when the EAR is readable and the partition does
not read back its lower-bank mirror; it warns when qspi-bootcopy differs from qspi-fsbl-uboot. (Up to v0.39-std 0af155a qspi-data was at 0x1100000, 15 MiB: a board updated from that layout gets a
freshly formatted /mnt/data, back it up first.)

## Install from stock firmware

Copy **pluto.frm and boot.frm together** to the Pluto's USB drive and eject. The stock update.sh
writes pluto.frm (qspi-linux, `fw_setenv fit_size`), then boot.frm (boot.bin to qspi-fsbl-uboot,
environment to qspi-uboot-env, after checking the mtd table), and reboots once. boot.frm = Tezuka
v0.3.24 Nano `BOOT.bin` (standard clocks) + `uboot/uboot-env-nexus.txt` + the plutosdr-fw mtd-info
block + md5 (`build-boot-frm.sh`). The environment boots with `fit_config=config@1` (Rev.B), takes
the FIT length from the FIT header (`fdt header get ... totalsize`; boot.frm overwrites the fit_size
pluto.frm just set), keeps the stock reset-cause DFU/RAM boot (`device_reboot sf|ram`), releases the
PS-PL level shifters/resets, and carries no network names or credentials. Writing boot.frm has no
fallback: a power loss during those seconds needs DFU/JTAG recovery. Later updates use the same
files (our update.sh keeps both handlers).

## Application slot (apps.frm)

Accepted only by this firmware (qspi-apps exists): copy `apps.frm` to the USB drive and eject.

    apps.frm = header (4096 bytes) | payload | [boot.bin] | md5 trailer (33 bytes)

- header: text lines, NUL padded to 4096 bytes; line 1 `NEXUSBS-APPS-FRM 1`, then `key=value`:
  `payload_size`, `payload_md5`, `bootbin_size` (0 = none), `bootbin_md5`, `bootbin_desc`,
  `version`, `created`.
- payload: a tar archive compressed with xz (`--check=crc32`), unpacked at boot to `/opt/apps`.
  Contract: `nexus/provision.sh <slot dir> /mnt/data` (idempotent, fills only missing files) and
  `nexus/start.sh` (the application, restarted after any exit).
- boot.bin: optional Zynq boot image (FSBL + U-Boot), at most 0xFF000 bytes.
- trailer: md5 of everything before it, hex + newline (the .frm convention).

`apps-frm-install` (called by update.sh): checks the trailer, header, sizes and payload md5; refuses
when the upper bank is not addressable (EAR unreadable, or the slot reads like its lower-bank
mirror); writes header + payload to qspi-apps (flashcp, then readback), and checks that the lower
bank (U-Boot environment, FIT) did not change. Only then a carried boot.bin is written: only the
64 KiB blocks that differ from the installed boot, qspi-bootcopy first, then qspi-fsbl-uboot from the
last block to block 0, each read back. Variants: `apps.frm` (no FSBL) and `apps-stock-clock.frm` (the Tezuka standard-clock FSBL, a
recovery path back from an overclock). The CPU/DDR clock is otherwise a setting (see "CPU/DDR
clock" below), not a separate file.
Exactly one `boot*.frm` and one `apps*.frm` may be on the drive. For the Tezuka FSBL variants only block 0 differs (36 bytes
of ps7_init data; U-Boot is byte-identical), so U-Boot is not rewritten and the unsafe window is one
64 KiB erase+program. /mnt/data is never touched.

At boot `S95apps` verifies the slot (header, payload md5) and unpacks it to `/opt/apps`, runs
`provision.sh`; `S97app` runs `start.sh` in `/mnt/data/nexus2` in the master role (a legacy
`/mnt/data/nexus2/app.conf` is then not started). An invalid or missing slot leaves a standalone SDR.
Built on CI by `.github/workflows/apps.yml` (`firmware/build-apps-frm.sh`): inputs are the assets of a
release in this repository (`nexus2`, `nexus-apps.tar.xz` = contrib/pluto/apps with `provision.sh` and
`start.sh` at its top level, `svxlink-root.tar.xz` = `bin lib share var etc/svxlink` at its top level,
`SHA256SUMS` for those three); the output is attached to release `apps-<tag>`. The build refuses any
secret-looking `key=value` that is not a placeholder.
`nexus-setup.txt` on the USB drive is copied to `/mnt/data/nexus2/setup.txt` (0600), removed from the
drive, and the Pluto reboots. `apps-slot-selftest --yes` is a one-off write test of the upper bank
(empty slot only).

## USB adapters (OTG host)

Standard Linux mechanisms only; nothing is loaded by board scripts.

- **Modules by alias.** Kernel modules are installed with `modules_install` (`build-modules.sh`), depmod
  writes `modules.dep`/`modules.alias`, and mdev's `$MODALIAS` rule runs `modprobe -b "$MODALIAS"` for
  every device (S10mdev replays the USB add events once at boot, like `udevadm trigger`).
- **AIC8800D80/D81 Wi-Fi** (`a69c:8d80` loader, `a69c:8d81`/`368b:8d81` running): the community driver
  `shenmintao/aic8800d80` branch `legacy-mcu1` (a9683a4, 2026-10-21), the maintained profile for
  `chip_mcu_id=1` units (SDK V3 firmware that fits the MCU1 upload window; the newer SDK V5 firmware on
  `main`/Radxa does not). `aic8800-intree.sh` copies it into `drivers/net/wireless/aic8800` and it is
  built **in tree** as `aic_load_fw.ko` + `aic8800_fdrv.ko` (no out-of-tree taint). `patches/aic8800`:
  firmware through `request_firmware()` from `/lib/firmware` (incl. `aic_userconfig_8800d80.txt`, whose
  TX power entries are capped at 15 dBm by the build), `aic_load_fw` binds only the vendor interface of
  the running device (no failed probe of the Bluetooth interfaces), errors at KERN_ERR and the
  chatter at debug level (`aicwf_dbg_level`, default errors only), standard cfg80211 regulatory by
  default (`custregd=0`: wireless-regdb, `iw reg set <country>`), `get_tx_power` reports the limit in
  force and `set_tx_power` never exceeds the userconfig table, the upstream rwnx_close fix for a dead
  bus (PR 99), and a firmware that stops answering commands resets the USB device through the USB core
  (`usb_queue_reset_device`) instead of leaving a crashed command queue. Options:
  `/etc/modprobe.d/aic8800.conf` (`ps_on=0`).
- **Driver CD mode** (`a69c:5721..572f`, Pandora `1111:1111`): `usb-storage` ignores these IDs (linux
  patch 0004, `US_FL_IGNORE_DEVICE`, so nothing is mounted), mdev calls `usb_modeswitch` with the
  standard configuration file `/etc/usb_modeswitch.d/<vid>:<pid>` (StandardEject), the adapter
  re-enumerates in Wi-Fi/Bluetooth mode.
- **Bluetooth**: kernel `btusb`; the AIC `368b:8d81` controller needs a zero-length packet after ACL
  bulk OUT transfers, now a `btusb` quirk-table entry (linux patch 0003, `BTUSB_ACL_TX_ZLP`) instead of
  the former kprobe module. BlueZ `bluetoothd` with the audio plugins (A2DP/AVRCP) and bluez-alsa
  (`S41bluealsa`, A2DP source, SBC, `bluealsa-cli`) on the system D-Bus; pairings in
  `/mnt/data/bt/lib` (`/var/lib/bluetooth`).
- Other Wi-Fi: Realtek r8188eu/rtl8xxxu/rtl8192cu (in kernel, `rtlwifi` firmware). Ethernet: r8152
  (`rtl_nic` firmware), ASIX, SMSC, CDC ECM/NCM, RNDIS.
- **Network**: `config.txt [WLAN] ssid_wlan/pwd_wlan/country_wlan` (U-Boot environment) give
  `/etc/wpa.conf` and the `wlan0` stanza of `/etc/network/interfaces` (S40network). Busybox ifupdown has
  no `allow-hotplug` keyword, so the stanzas are `auto` (boot) and mdev runs the stock
  `/lib/mdev/ifupdown.sh` (`ifup`/`ifdown`) when an adapter appears or goes; wpa_supplicant is started
  and stopped by its ifupdown hook, `udhcpc` sends the host name (`hostname`) and stays in the
  background until it gets a lease (`udhcpc_opts -b`). DNS: busybox's upstream `simple.script` as
  `/usr/share/udhcpc/default.script` (plain `nameserver` lines). mDNS: avahi, `<hostname>.local`
  (default host name `plutosdr`).
- Lockups: soft-lockup detector (reports after 8 s, `kernel.watchdog_thresh=4`) and hung-task
  detector (20 s) log a stack trace; the hardware watchdog resets after 10 s.

## nexus-config (all Pluto settings, raspi-config style)

Every setting of the Pluto itself is made with one tool on the device, over SSH or the serial console
(any TERM; an unknown one falls back to vt100; laid out for 80x24). The NexusBS dashboard only
configures NexusBS. The tool writes what the existing mechanisms read, so they all keep working: the
U-Boot environment (the config.txt keys: `hostname`, `operating_mode`, `usb_mode`, `xo_correction`,
`cpu_clock`, `cpu_clock_cooling_confirmed`, `country_wlan`, `bluetooth`), `nexus-role`,
`nexus-clock`, `nexus-net` with `/mnt/data/network/network.json`, and the NexusBS provisioning in
`/mnt/data/nexus2`.

    nexus-config                                  interactive menu (dialog)
    nexus-config nonint [OPTIONS] FUNCTION [ARGS] one action, no prompts
    nexus-config get ITEM                         print a current value
    nexus-config help                             every function

Exit codes of `nonint`: 0 done, 1 refused or failed (the reason on stderr; nothing was changed),
2 usage, 3 network settings applied but waiting for `do_net_confirm` (see "Rollback"). Actions are
serialised by a lock (`/run/nexus-config.lock`). Each action applies at once where it can, persists,
and reports; nothing is lost on reboot.

**Secrets** (Wi-Fi passphrases, `DASHBOARD_PASSWORD`, `DMR_PASSWORD`, `BM_API_KEY`, `TETRA_PASSWORD`,
`SVXLINK_AUTH_KEY`) are never command-line arguments (so never in `ps`), never printed or logged: the
menu uses a hidden input box, `nonint` reads them from stdin and refuses a terminal there (no prompt,
no echo). Inside the tool they travel through the environment of `jq` only (readable by root alone).
network.json, wpa_supplicant.conf (derived 256-bit keys only), setup.txt and backups are 0600.

### Menu and functions

| menu | nonint | notes |
|---|---|---|
| 1 System: Host name | `do_hostname NAME` | U-Boot `hostname` + `/etc/hostname` now, avahi restarted (`NAME.local`); DHCP leases carry it from their next renewal. network.json no longer holds a host name |
| Station role | `do_role master\|slave` | `nexus-role` (USB composite rebuilt, NexusBS started/stopped) |
| USB mode | `do_usb_mode peripheral\|auto\|host` | next boot; `host` = a computer on USB-C no longer sees the Pluto |
| Crystal correction | `do_xo_correction HZ\|default [--now]` | the measured 40 MHz reference in Hz, 8 digits within 250 ppm (39990000-40010000); shown as Hz and ppm, ppm = (xo - 40e6) / 40e6 x 1e6 (39999331 = -16.725 ppm); next boot, `--now` also writes the AD9361 (restart NexusBS then) |
| CPU/DDR clock | `do_cpu_clock VARIANT [--cooling-confirmed]` | `standard` (default, recommended), `495_396` and `950_600` (Tezuka files, not tested on the reference board), `1100_750` (tested on the reference board with heatsinks and a fan only; above the -1 speed grade spec; active cooling required: refused without `--cooling-confirmed`, the menu asks explicitly). The boot block is rewritten at the next restart: a power cut during that < 1 s write needs DFU/JTAG recovery. The menu offers the restart |
| Reboot / Shut down | `do_reboot`, `do_shutdown` | |
| 2 Network: Status | `get network` (JSON) | IP per interface, gateway, DNS, SSID, signal, pending change |
| Wi-Fi networks | `get wifi` | SSID, priority, IPv4 mode, open, default (highest priority), connected; never a passphrase |
| Add / change a Wi-Fi network | `do_wifi_add SSID [--open] [--priority N]` | passphrase on stdin (8-63 printable ASCII or 64 hex); an existing SSID is updated. The first network change imports the config.txt Wi-Fi/Ethernet settings into network.json; from then on those config.txt keys are ignored (config.txt says so) |
| Remove | `do_wifi_remove SSID` | |
| Preferred network | `do_wifi_default SSID`, `do_wifi_priority SSID N` | default = highest priority (0-1000) |
| Wi-Fi network IPv4 | `do_wifi_ipv4 SSID dhcp \| static ADDR/PREFIX [GATEWAY\|-] [DNS[,DNS]]` | per SSID: applied when that network connects |
| Ethernet IPv4 | `do_eth_ipv4 dhcp \| static ADDR/PREFIX [GATEWAY\|-] [DNS[,DNS]]` | |
| DNS override | `do_dns default \| DNS[,DNS[,DNS]]` | replaces the DHCP servers; `default` takes effect at the next lease |
| Regulatory country | `do_wifi_country CC` | U-Boot `country_wlan`, `iw reg set` now, wpa_supplicant country |
| Confirm / Roll back | `do_net_confirm`, `do_net_rollback`, `do_net_apply` | |
| 3 Bluetooth: on/off | `do_bluetooth on\|off` | U-Boot `bluetooth` (config.txt `bluetooth`), controller powered now, `AutoEnable` in main.conf, the NexusBS Player switch (`player.json bt_off`) follows; off = powered off from every boot (`S39bluetooth`). The Player's own switch uses the same controller: switching it on there works until the next boot |
| Controller and paired devices | `get bt` | |
| 4 NexusBS: Status | `get station` | role, application slot version, running, callsign, dashboard URL, last setup result |
| Station identity | `do_station_set KEY [VALUE]`, `do_station_setup` (KEY=value lines on stdin) | keys `CALLSIGN TRANSMIT DASHBOARD_USER DASHBOARD_PASSWORD DMR_ID DMR_NETWORK_ID DMR_PASSWORD BM_MASTER BM_API_KEY TETRA_ISSI TETRA_PASSWORD TETRA_HOST P25_TALKGROUP SVXLINK_CALLSIGN SVXLINK_AUTH_KEY SVXLINK_HOSTS XO_CORRECTION` (secret ones on stdin only). Written to `/mnt/data/nexus2/setup.txt` (0600), then `/opt/apps/nexus/bin/nexus2 provision --data /mnt/data/nexus2 --defaults /opt/apps/nexus/defaults`; setup.txt is removed afterwards whatever the outcome, `setup.result` is shown, an `XO_CORRECTION` goes to the U-Boot env. Restart NexusBS to use it |
| Restart NexusBS | `do_station_restart` | `/etc/init.d/S97app restart` (master role only) |
| Dashboard login | `get station_login` | location of `/mnt/data/nexus2/dashboard-login.txt` (generated password; not displayed) |
| NexusBS log | `do_station_log [LINES]` | tail of `/var/log/nexus.log` |
| 5 Update: Versions | `get versions` | firmware, application slot, U-Boot, FSBL clock variant, kernel |
| How to update | | the USB drive `.frm` procedure (pluto.frm, boot.frm, apps*.frm, nexus-setup.txt) |
| Backup /mnt/data | `do_backup PATH\|-` | tar.gz (0600; holds passwords and keys) to a file or directory (a stick under /media, or /tmp = RAM), or to stdout: `ssh root@pluto.local nexus-config nonint do_backup - > pluto-data.tar.gz`. Restore: `tar -xzf FILE -C /mnt/data` and reboot |
| Factory reset | `do_factory_reset ERASE-ALL-DATA` | erases /mnt/data (network, NexusBS station, pairings, SSH host keys; the U-Boot env stays); the menu asks twice. Reboot afterwards |
| 6 About / help | `help` | |

`get` items: `hostname role usb_mode xo_correction xo_ppm cpu_clock clock bluetooth bt country wifi
network dns ethernet net_pending station station_login station_result versions all`.

### Rollback of network changes

When the SSH session runs over an interface the change restarts (its local address belongs to eth0 or
wlan0 and that interface changes), the change is applied with a 60 s automatic rollback: the menu asks
"Keep the new settings?" (a Yes within 55 s keeps them; No or no answer restores the previous ones at
once); `nonint` exits 3 and prints the rollback time, `nexus-config nonint do_net_confirm` keeps the
change. The timer survives the SSH connection dropping. Over the serial console or the USB network no
rollback is needed. `--rollback S` forces one, `--no-rollback` skips it, `--no-apply` only saves
network.json (apply later with `do_net_apply`). While a change waits for confirmation every further
network change is refused.

### Examples

    nexus-config nonint do_hostname nexus-yo3tco
    read -rs P; printf '%s\n' "$P" | nexus-config nonint do_wifi_add "Home WiFi"; unset P
    nexus-config nonint do_wifi_ipv4 "Home WiFi" static 192.168.1.119/24 192.168.1.1 192.168.1.1
    nexus-config nonint do_wifi_default "Home WiFi"
    nexus-config nonint do_xo_correction 39999331
    nexus-config nonint do_cpu_clock 1100_750 --cooling-confirmed    # heatsinks + fan only
    nexus-config nonint do_station_set CALLSIGN YO3TCO
    printf '%s\n' "$DMR_PW" | nexus-config nonint do_station_set DMR_PASSWORD
    nexus-config nonint do_station_setup < nexus-setup.txt && nexus-config nonint do_station_restart
    ssh root@pluto.local nexus-config nonint do_backup - > pluto-data.tar.gz

### config.txt on the USB drive

config.txt is regenerated from the U-Boot environment at every boot, so the drive shows values set
with nexus-config after the next boot. Until then a config.txt edited on the drive still works without
reverting nexus-config: update.sh compares the saved file with the copy S45msd put on the drive
(`/opt/config.drive`), and every one of the keys above that the operator did not change keeps its
current environment value. An invalid or empty `operating_mode`, `usb_mode`, `cpu_clock`,
`cpu_clock_cooling_confirmed` or `bluetooth` also keeps the current value (an old config.txt without
those lines changes nothing).

### Tests

`firmware/tests/nexus-config-test.sh` runs every nonint function in a sandbox (`NEXUS_ROOT` prefixes
the file paths of nexus-config, nexus-net, nexus-net-wifi-event, nexus-clock, nexus-role,
apps-flash-lib and S39bluetooth; fake fw_printenv/fw_setenv, mtd files, ip, wpa_cli, ifup,
bluetoothctl, nexus2 provision and init scripts on PATH), including the refusals (1100_750 without
cooling, invalid host name/IP/prefix/SSID/passphrase/priority/country, aliased or unreadable upper
flash bank, foreign boot image, a change while another waits, rollback without confirmation) and a
check that no secret reaches any command line (every fake and jq record their argv), the log or the
output. `--docker` runs it in a throwaway alpine:3.20 container (busybox 1.36.1 as in the image, jq
installed in the container); `STOCK_BOARD_DIR=<ADI buildroot board/pluto>` adds update.sh's
config.txt handling after the post-build.sh patches.

    firmware/tests/nexus-config-test.sh --docker

## Station role: slave (default) or master

`config.txt` [SYSTEM] `operating_mode = slave|master` (U-Boot environment `operating_mode`), or
`nexus-role slave|master` on the device; applied at once and at every boot, shown at login.

- **slave** (default): the Pluto is the SDR of a NexusBS running elsewhere, e.g. a Raspberry Pi.
  Both timestamp SDR gadgets run: USB FunctionFS (`sdr_usb_gadget`, `uri=usb:...,direct=1`) and
  UDP (`sdr_ip_gadget` 30432/30433, `uri=ip:<pluto>,direct=1,timestamp_every=...`), plus iiod.
  No application is started.
- **master**: NexusBS runs on the Pluto (S97app: the application slot, or `/mnt/data/<name>/app.conf`) and uses the radio
  directly; both SDR gadgets are stopped and the USB SDR function is left out of the composite.
  Switching rebuilds the USB composite, so a USB link drops for a few seconds.
- `S97app start` is idempotent (a running station is not started twice when `nexus-role master` or
  update.sh calls it again); `S97app restart` stops and starts it.

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

## CPU/DDR clock (nexus-clock)

The clock is the FSBL's ps7_init PLL data in the first 64 KiB block of qspi-fsbl-uboot. The image
carries block 0 of each Tezuka v0.3.24 Nano boot image and the rest of the standard BOOT.bin
(`/usr/share/nexus-clock`, `build-clock-blocks.sh`; the build fails unless the variants differ only
inside block 0). Setting: config.txt `[SYSTEM] cpu_clock` = U-Boot env `cpu_clock`, default `standard`
when unset or invalid:

| cpu_clock | |
|---|---|
| `standard` | Tezuka default clocks |
| `495_396` | CPU 495 MHz / DDR 396 MT/s: Tezuka file, not tested on the reference board |
| `950_600` | CPU 950 MHz / DDR 600 MT/s: Tezuka file, not tested on the reference board |
| `1100_750` | CPU 1100 MHz / DDR 750 MT/s: tested on the reference board (HamGeek Pluto Nano Rev.B) with heatsinks and a fan installed, ran for days; above the -1 speed grade spec, active cooling required. Needs `cpu_clock_cooling_confirmed = yes`, otherwise refused, logged, and `standard` is installed instead |

`S22clock` (`nexus-clock apply`, at boot, after the upper-bank checks of S21data) compares block 0
with the configured variant; identical = no flash write. Otherwise, provided the rest of the installed
boot image is Tezuka's: the new 1 MiB boot image goes to qspi-bootcopy first (read back), block 0 of
qspi-fsbl-uboot is erased and written (read back; on a mismatch the saved block is written back at once
and CRITICAL is logged), and the Pluto reboots once. A power cut during that < 1 s write needs DFU/JTAG
recovery. `nexus-clock status` (JSON: configured, effective (what apply installs), installed,
cooling_confirmed, ARM/DDR PLL and the resulting CPU MHz / DDR MT/s from the SLCR registers),
`nexus-clock set VARIANT [--cooling-confirmed]` (environment only). Changing cpu_clock in config.txt
reboots once to apply it. apply also refuses while update.sh has the USB drive image mounted
(`/mnt/msd`), and erases by the partition's real erase block size.

## Network settings: network.json (nexus-net)

`/mnt/data/network/network.json` (0600, written by `nexus-config`) overrides the config.txt /
U-Boot Wi-Fi and Ethernet settings when it exists (S40network runs `nexus-net render-boot` before
`ifup -a`; an invalid file is ignored and logged). Without it the stock config.txt behaviour stays.

    {"schema": 1, "hostname": "plutosdr", "dns": ["1.1.1.1"],
     "wifi": {"networks": [{"ssid": "...", "psk": "...", "priority": 10, "ipv4": {"mode": "dhcp"}},
                           {"ssid": "...", "psk": "...", "priority": 5,
                            "ipv4": {"mode": "static", "address": "192.168.1.119/24",
                                     "gateway": "192.168.1.1", "dns": ["192.168.1.1"]}}]},
     "ethernet": {"ipv4": {"mode": "dhcp"}}}

`hostname`, `dns` (global list, replaces DHCP DNS), `wifi`, `ethernet` and `psk` (8-63 characters or
64 hex digits; absent/empty = open network) are optional. Every network goes into
`/etc/wpa_supplicant.conf` (0600) with its priority (highest = preferred), the SSID hex-encoded, the
PSK as the derived 256-bit key (`wpa_passphrase`, the passphrase is never written), `id_str "netN"`
and the country of config.txt `country_wlan` (default RO). wlan0 is `inet manual`: `wpa_cli -a
/usr/sbin/nexus-net-wifi-event` applies `/run/nexus-net/wifi/netN.conf` on CONNECTED (udhcpc with the
host name, or the static address, default route and DNS) and drops it on DISCONNECTED, so each SSID
keeps its own IPv4 settings when the Pluto moves between networks. eth0 is a normal ifupdown stanza.

    nexus-net status                 JSON: hostname, IPv4 and mode per interface, gateway, DNS, SSID, signal
    nexus-net validate FILE          exit 1 with "field.path: reason" (values are never printed)
    nexus-net apply [--rollback S]   render, restart only eth0/wlan0 if their settings changed; with
                                     --rollback the previous files (and network.json) return after S s
                                     (the timer runs in its own session with SIGHUP ignored, so it
                                     survives the SSH connection it is meant to rescue); refused while
                                     an earlier apply waits for confirmation
    nexus-net confirm                keep the applied settings
    nexus-net revert                 return to the settings before the pending apply now
    nexus-net plan                   print the interfaces an apply would restart (nothing changes)

## USB-C role, SSH host keys, Bluetooth

- config.txt `[SYSTEM] usb_mode = peripheral|auto|host` (U-Boot env `usb_mode`, default peripheral)
  is written into config.txt at boot, read back on eject like `operating_mode`, and takes effect at
  the next boot; S96usbrole prints the mode at boot. A Wi-Fi stick directly on the Pluto needs `auto`
  (or `host`).
- SSH host keys: `/etc/dropbear` links to `/mnt/data/dropbear`; dropbear generates the keys once
  (`-R`) and they survive reboots and firmware updates (stock keys saved with device_persistent_keys
  in /mnt/jffs2 are still restored by S21misc).
- `/etc/bluetooth/main.conf`: `[Policy] AutoEnable=true`, so an adapter that enumerates late (after
  the USB role switch) is powered at once. config.txt `[SYSTEM] bluetooth = on|off` (U-Boot env
  `bluetooth`, default on; `nexus-config`): with off, `S39bluetooth` (before bluetoothd) sets
  `AutoEnable=false` in the RAM copy of main.conf and `bt_off` in the NexusBS Player's
  `/mnt/data/nexus2/player.json`, so the controller stays powered off from boot on.

## SSH, SCP, SFTP

Dropbear (`dropbearmulti`: server, client, `scp`) and the Green End SFTP server as
`/usr/libexec/sftp-server`: `scp`, `sftp` and `rsync`-free copies from any OpenSSH client work as usual
(OpenSSH 9+ `scp` uses the SFTP protocol; `scp -O` uses dropbear's scp).

## Configuration files shipped in the image

Generic and commented, without secrets: `/etc/pluto/config.txt.example` (the `config.txt` keys) and
`/etc/nexus2/nexus2.example.toml` (NexusBS station example). Applications, their live configuration
and the Bluetooth pairings stay on `/mnt/data` (UBIFS), which survives firmware updates.

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
