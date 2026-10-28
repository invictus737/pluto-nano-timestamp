# Nexus-BS RTOS for the Pluto Nano

A Linux-free image for the HamGeek Pluto Nano (Zynq-7010 + AD9363A): RTEMS 6.2 runs the full
Nexus-BS 2.0 station (TETRA, DMR, P25, analog FM, networks, dashboard) on both Cortex-A9 cores,
boots to its shell in about 5 s (most of it U-Boot) and keeps the radio path on its own core.
Branch `Nexus-BS-2.0_RTOS`; design and milestones in [PLAN.md](PLAN.md).

## Status (2026-10-28)

| Part | State |
|---|---|
| Boot from QSPI, both cores, stock 666 MHz, PL bitstream | working on the board |
| USB host (Zynq EHCI, ULPI PHY, VBUS on the OTG port) | working on the board |
| AIC8800 storage-mode switch | working on the board |
| AIC8800 Wi-Fi station (WPA2), USB-Ethernet drivers, SSH, config store | built, board test pending |
| Native AD9363 radio driver (SoapySDR `plutosdr` API) | in development |
| nexus2 station on RTEMS | compiles for RTEMS; linking and on-air test pending |

## Compatible USB network adapters

RTEMS has no Linux driver collection: only the adapters below work, because only their drivers
are in the image (FreeBSD drivers from rtems-libbsd, plus the AIC8800 port). Anything else is
ignored. Connect them to the Pluto's USB-C data port with an OTG adapter or a powered hub; the
Pluto supplies VBUS on that port.

"Validated" means tested on this board with this image. "Driver included" means the driver is in
the image and the chipset is supported by it, but no unit has been tested on the Pluto yet.

### Wi-Fi

| Chipset (USB ID) | Wi-Fi | BT | Driver | State |
|---|---|---|---|---|
| AICSemi AIC8800DC / DW (a69c:88dc), "AX300 / WiFi 6" sticks | 2.4 GHz | no | aicwf (RT-Smart port) | storage switch validated, Wi-Fi pending |
| AICSemi AIC8800D80 / D81 / D84 / D85 (a69c:8d80, 8d81, 368b:8d81), "AX900 / WiFi 6 + BT 5.x" | 2.4 + 5 GHz | yes (HCI transport, host stack later) | aicwf | driver included |
| AICSemi AIC8800 / 8801 (a69c:8800, 8801) | 2.4 GHz | yes | aicwf | driver included |
| Realtek RTL8188EU(S) | 2.4 GHz | no | rtwn (libbsd) | driver included |
| Realtek RTL8192CU | 2.4 GHz | no | rtwn | driver included |

AIC8800 sticks start as a "driver CD" (USB storage); the image switches them to Wi-Fi by itself.
Transmit power is capped at 15 dBm in the embedded firmware configuration.

### Wired Ethernet (USB)

| Chipset | Speed | Driver | State |
|---|---|---|---|
| Realtek RTL8156 / RTL8156B (e.g. Dell and other "USB-C 2.5G" adapters) | 2.5 Gb/s | ure | driver included |
| Realtek RTL8153 / RTL8152 | 1 Gb/s / 100 Mb/s | ure | driver included |
| ASIX AX88179 / AX88178A | 1 Gb/s | axge | driver included |
| ASIX AX88772 / AX88772B | 100 Mb/s | axe | driver included |
| Any CDC-ECM / CDC-NCM class adapter | varies | cdce | driver included |

Speeds are those of the adapter; the Pluto's USB 2.0 port limits real throughput to roughly
200-300 Mb/s.

### Where to buy

The links are plain Amazon searches by chipset or model. **There is no affiliate tag and no
affiliation with any seller**; they are listed only because these chipsets are the ones this
RTOS image supports, and the set is deliberately small. Check the chipset in the listing (or on
the box) before buying: the same product name is sometimes sold with a different chip.

- AIC8800DC Wi-Fi stick: <https://www.amazon.com/s?k=AIC8800DC+USB+WiFi+adapter>
- AIC8800D80 Wi-Fi + Bluetooth stick: <https://www.amazon.com/s?k=AIC8800D80+USB+WiFi+Bluetooth+adapter>
- RTL8188EUS Wi-Fi (e.g. TP-Link TL-WN725N): <https://www.amazon.com/s?k=TP-Link+TL-WN725N>
- RTL8156B 2.5G Ethernet: <https://www.amazon.com/s?k=RTL8156B+USB+2.5G+Ethernet+adapter>
- RTL8153 Gigabit Ethernet (e.g. TP-Link UE300): <https://www.amazon.com/s?k=TP-Link+UE300>
- AX88179 Gigabit Ethernet: <https://www.amazon.com/s?k=AX88179+USB+Gigabit+Ethernet+adapter>
- USB-C OTG adapter: <https://www.amazon.com/s?k=USB-C+OTG+adapter+USB-A+female>
- Powered USB hub (when an adapter needs more current than the Pluto's port provides):
  <https://www.amazon.com/s?k=powered+USB+2.0+hub+with+power+adapter>

## Configuration

Settings live in the QSPI data partition (`/data`, JFFS2). To change them, put `config.txt`
and/or `nexus2.toml` on a FAT USB stick and plug it into the Pluto: they are applied, kept, and
deleted from the stick (secrets are never printed). Format and all keys:
[config/README.md](config/README.md), example [config/config.txt.example](config/config.txt.example).

## Access

- Serial console (UART1, 115200 8N1): the RTEMS shell (`help`, `ifconfig`, `config show`,
  `wifi status`, `cpuuse`).
- SSH on port 22 and the dashboard on port 8080 share one login, `[dashboard.auth]` in
  nexus2.toml (default user `admin`, password `nexus`; change it). Public keys from
  `ssh/authorized_keys` are accepted as well.

## Recovery

U-Boot, its DFU mode and the Linux backup are untouched. To go back to Linux, restore the backup
with `run dfu_sf` + `dfu-util` and set `fit_config` / `fit_size` back (steps in PLAN.md, "Board
state").
