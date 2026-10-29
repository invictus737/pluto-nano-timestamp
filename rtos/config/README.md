# Nexus-BS RTOS configuration (Pluto Nano)

Persistent settings of the RTEMS image: a JFFS2 file system in QSPI flash mounted at `/data`,
the key/value store `/data/nexus.conf`, a `config.txt` / `nexus2.toml` drop-in on a USB drive,
mirroring of the boot-relevant keys into the U-Boot environment, and the `config` shell command.

Code: `rtos/app/config_store.c` (RTEMS side), `rtos/app/config_kv.c` (parser, merge, masking,
export, CRC-32, U-Boot env format; portable), `rtos/app/qspi_nor.c` (flash access).
Tests: `rtos/config/test/run.sh [uboot-env.bin]` (host `cc`, ASan/UBSan).

## Wiring (main.c)

```c
#include "config_store.h"
...
nexus_config_init();            /* BEFORE rtems_bsd_initialize(): the media server must be
                                   listening when the USB drive attaches at boot */
rtems_bsd_initialize();
...
nexus_config_shell_register();  /* any time; adds the `config` shell command */
```

Consumers: `nexus_config_get("hostname")`, `nexus_config_wifi(i, &ssid, &psk)` (i = 0..4 in
priority order), `nexus_config_on_change(cb)` (e.g. Wi-Fi reconnects), `nexus_config_station_dir()`
= `/data/nexus2`, `nexus_config_station_rollback()` when nexus2 rejects a new `nexus2.toml`.
`[ACTIONS]` flags are stored as `action_<name> = 1`; the consumer clears its flag with
`nexus_config_set("action_calibrate", NULL)`.
Pointers returned by `nexus_config_get`/`nexus_config_wifi` stay valid for the life of the system.

## Flash layout used

| QSPI | size | use here |
|---|---|---|
| 0x000000 | 1 MiB | FSBL + U-Boot: never written |
| 0x100000 | 128 KiB | U-Boot environment: read; written only for the keys below, with backup |
| 0x120000 | 896 KiB (14 x 64 KiB) | "nvmfs": JFFS2 `/data` |
| 0x200000.. | | firmware FIT, bootcopy, apps, UBI data: never written |

Writes and erases outside 0x100000..0x1FFFFF are refused by `qspi_nor.c`. The flash is accessed
with the W25Q256's explicit 4-byte-address opcodes (0x13 read, 0x12 program, 0xDC 64 KiB erase):
the 3/4-byte addressing mode and the extended address register are never changed. SCLK is set
to <= 25 MHz. Every program and erase is verified by reading back.

`/data` holds: `nexus.conf` (0600), `nexus2/nexus2.toml` (+ `.bak`), `ssh/authorized_keys`
(from the stored keys, OpenSSH format), `uboot-env.orig` (the environment before the first
change), `uboot-env.bak` (before the last change), `.nexus-rtos` (marker). If `/data` does not
mount, the region is erased and formatted once; if the flash cannot be used at all, the
configuration lives in RAM for that boot (console says so).

## config.txt

Full example with every key: [config.txt.example](config.txt.example). Same layout as the Pluto
Linux firmware's `config.txt`:

- `[SECTION]` headers, `key = value`, `#` or `;` comment lines (a `#` inside a value is data),
  case-insensitive keys/sections, CRLF and a UTF-8 BOM accepted, optional `"..."` quotes.
- Only the keys present change. An empty value = back to the default (key removed).
- `********` as a value keeps the stored secret (so an exported file can be edited and
  dropped back).
- Invalid values are rejected with a console warning and not applied (secret values are never
  printed). Unknown keys are kept and warned about.

Sections and keys (canonical names = the Linux U-Boot env names):

| section | key | values | U-Boot env |
|---|---|---|---|
| NETWORK | hostname | letters, digits, `-`, <= 63 | yes |
| NETWORK | ipaddr, ipaddr_host, netmask | gadget addresses | yes |
| NETWORK | dns | up to 3 IPv4, space/comma separated | |
| WLAN | ssid_wlan, pwd_wlan | network 0 (highest priority) | |
| WLAN | ssid_wlan1..4, pwd_wlan1..4 | networks 1..4, lower priority by number | |
| WLAN | ipaddr_wlan, gateway_wlan | empty = DHCP; `a.b.c.d[/prefix]` | ipaddr_wlan |
| WLAN | country_wlan | ISO alpha-2 | yes |
| USB_ETHERNET | ipaddr_eth, netmask_eth, gateway_eth | empty = DHCP | ipaddr_eth, netmask_eth |
| SYSTEM | xo_correction | Hz, 39990000..40010000 | yes |
| SYSTEM | operating_mode | slave, master, light | yes |
| SYSTEM | usb_mode | peripheral, auto, host | yes |
| SYSTEM | cpu_clock | standard, 495_396, 950_600, 1100_750 | yes |
| SYSTEM | cpu_clock_cooling_confirmed | yes, no | yes |
| SYSTEM | bluetooth | on, off | yes |
| SYSTEM | nexus_layout | light (default: CPU1 = DSP/protocol, CPU0 = SDR I/O, network, dashboard), cpu1 (whole station on CPU1), float (priorities only) | |
| SYSTEM | usb_ethernet_mode, udc_handle_suspend | rndis/ncm/ecm, 0/1 | yes |
| SSH | authorized_key | OpenSSH public key line, repeatable (<= 8 stored) | |
| ACTIONS | reset, export | 1 = once (reboot after apply / write config-current.txt) | |
| ACTIONS | calibrate, diagnostic_report, dfu | 1 = stored flag `action_<name>` | |

Wi-Fi passphrases: 8..63 printable characters or 64 hex digits; empty = open network. Removing
`ssid_wlanN` removes its passphrase. Accepted aliases: `wifi_ssid`/`wifi_psk`,
`wifiN_ssid`/`wifiN_psk`, and a short `[wifi]` section (`ssid`, `psk`/`password`, `ssidN`,
`pskN`, `country`, `ipaddr`, `gateway`).

**Login password**: SSH and the dashboard share `[dashboard.auth] username/password` in
`nexus2.toml` (defaults admin / nexus). `config.txt` has no password key; a `[SSH] password`
line is ignored with a warning. Change the password by putting a `nexus2.toml` on the drive.

Example:

```ini
[WLAN]
ssid_wlan = HomeNet
pwd_wlan = correct horse battery
ssid_wlan1 = Phone hotspot
pwd_wlan1 = hotspot-pass
[SYSTEM]
operating_mode = master
xo_correction = 39999331
[SSH]
authorized_key = ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAI... operator@laptop
[ACTIONS]
reset = 1
```

## USB drive drop-in

The libbsd media server mounts FAT volumes of USB mass storage devices at `/media/...`. On
every mount (at boot or hot-plugged) the firmware looks, case-insensitively, at the top level
and in a `nexus/` folder for:

1. `nexus2.toml`: copied to `/data/nexus2/nexus2.toml` (atomic; the previous file is kept as
   `nexus2.toml.bak`), then deleted from the drive.
   `config: station nexus2.toml installed (N bytes) from <path>, previous kept as nexus2.toml.bak, file removed`
   nexus2 validates it later; if it rejects it, `nexus_config_station_rollback()` (or
   `config station rollback`) restores the `.bak`. Calibration (`nexus2.calibration.toml`) is
   not handled here: it ships with the firmware.
2. `config.txt`: parsed, merged into `/data/nexus.conf` (temp file, fsync, rename), U-Boot keys
   mirrored, the file deleted and the drive synced.
   `config: applied N keys from <path>, file removed (C changed[, U unknown kept, I invalid, G ignored])`
   If the store cannot be written the file stays: `config: <path> not applied (storage error), file kept`.
3. `[ACTIONS] export = 1`: `config-current.txt` written to the drive (secrets masked).
4. `[ACTIONS] reset = 1`: reboot after everything is written.

Other console messages: `config: /data mounted (jffs2 on qspi 0x120000+0xe0000, N KiB free)`,
`config: N keys from /data/nexus.conf`, `config: Wi-Fi network "X" seeded from the build
defaults`, `config: U-Boot environment updated (N variables), previous image in
/data/uboot-env.bak`, `config: line L: invalid KEY (value not shown), ignored`,
`config: line L: unknown key KEY, kept`, and `qspi: W25Q256 id ef4019 sr1 .. sr3 .. ear ..`.

## U-Boot environment

Keys marked "U-Boot env" above are also written to the environment at 0x100000 (non-redundant,
128 KiB: CRC-32 LE + `name=value\0` list + `\0`, padding byte kept from the existing image) so
U-Boot and a Linux recovery boot see the same values. Procedure: read the 128 KiB, refuse if the
CRC is bad, apply only changed keys (an empty value deletes the variable), stop if nothing
changed, save `/data/uboot-env.orig` (first time) and `/data/uboot-env.bak`, erase the two
64 KiB sectors, program, verify; on a failed write the previous image is written back. Boot
variables (`bootcmd`, `fit_config`, `fit_size`, ...) are never touched, and Wi-Fi SSIDs and
passphrases are not copied into the environment.

Power loss during the ~1 s write leaves a bad CRC: U-Boot then uses its built-in default
environment. Recovery: DFU (`run dfu_sf`), `dfu-util -a uboot-env.dfu -D uboot-env.bak` (copy it
from `/data` over SFTP first, or use the backup taken before the RTEMS flash).

## Build-time Wi-Fi default

When `/data/nexus.conf` has no Wi-Fi network, the first boot seeds `ssid_wlan`/`pwd_wlan` from
the CI secrets `NEXUS_WIFI_SSID` / `NEXUS_WIFI_PSK` (Image step env). `rtos/config/gen-wifi-defaults.sh`
writes `nexus_wifi_defaults.h` into the build directory only (bytes XOR 0x5a: keeps the
passphrase out of a plain `strings`, not encryption: the image and ELF artifacts carry it).
Empty secrets: no default.

## Shell

```
config [show]                    all keys (secrets masked), /data free space
config get KEY | set KEY VALUE... | unset KEY
config wifi list | add SSID PSK|- | del SLOT|SSID   (slot 0 = highest priority)
config apply FILE                apply a config.txt from any path (file kept)
config export [DIR]              DIR/config-current.txt (default: the last USB drive)
config station rollback          nexus2.toml.bak -> nexus2.toml
config format yes                erase /data and rewrite the configuration
```
