# pluto-nano-timestamp

Timestamping firmware for the **HamGeek NanoSDR / "Pluto Nano"** (Zynq XC7Z010**CLG400** + AD9363,
512 MB DDR3, 32 MB QSPI, USB only), built from the ADI PlutoSDR firmware with the
sample-timestamping work of [pgreenland/plutosdr-fw](https://github.com/pgreenland/plutosdr-fw)
(`v0.38_timestamp`), and the board support of [F5OEO/tezuka_fw](https://github.com/F5OEO/tezuka_fw)
(`nano`).

The stock ADALM-Pluto images, pgreenland's included, are built for the **CLG225** package. The Nano is
CLG400: same die, different balls. A CLG225 bitstream configures on the Nano but drives the wrong pins.
This repository exists because the vendor ships no schematic, no XDC and no sources.

## Status

| Step | State |
|---|---|
| Identify the board and its FPGA pinout from the vendor bitstream | **done** (see below) |
| Nano device tree (USB PHY reset GPIO 46, 40 MHz refclk, LED on gpio0 15) and PS7 settings (clg400, USB reset MIO 46, UART1 MIO 12/13, DDR MT41K256M16 x16) | taken from tezuka / F5OEO maia-sdr `nano` |
| Port-name → ball constraint file (`system_constr.xdc`) for the Nano | **done**: `board/nano/system_constr.xdc` from F5OEO/maia-sdr (refactor, `boards/nano`), 47 of the vendor's 53 pads mapped to ADI port names with matching direction (`data/vendor-nano-port-map.csv`); 6 vendor-only pads unconstrained |
| ADI `hdl` pluto project retargeted to `xc7z010clg400-1` + Quantulum timestamp IP | **scripted**: `firmware/apply-nano.sh` (part, PS7 clg400 / bank0 3.3 V / USB reset MIO 46 / DDR, constraints, DT); not yet synthesised |
| `plutosdr-fw` v0.38_timestamp build with the Nano bitstream and DT | `firmware.yml`: Vivado 2022.2 is installed **inside the GitHub-hosted job** (Zynq-7000 only), HDL built, then the FIT assembled in a second job. Needs two things from the repo owner: secrets `AMD_USER`/`AMD_PASS` and the web installer uploaded as release `vivado-installer`. First run pending |
| DFU flash and `SoapySDRUtil --probe ... Timestamps: YES` | todo |

## How the pinout was recovered

`tools/frames.py` reconstructs the configuration frames of a compressed 7-series bitstream
(FDRI bursts + MFWR replication). `tools/iob_decode.py` evaluates the IOB33 features of the
[prjxray](https://github.com/f4pga/prjxray-db) Zynq-7000 database per pad and maps sites to package
balls. `tools/compare_pins.py` classifies pads as inputs (IBUF features) or outputs (DRIVE features).

Validation: the official ADI v0.39 Pluto bitstream (CLG225) decodes to exactly the 54 PL pins of ADI's
`projects/pluto/system_constr.xdc`, no extra, no missing.

Result on the vendor firmware (`v0.39-dirty`, HDL `dev_prj_2018_r1-2241-g065c8-dirty`, built
2025-09-15, Vivado 2023.2, part `7z010clg400`): **53 used pads, 18 inputs, 35 outputs**
(`data/vendor-nano-used-pins.csv`). Compared with the prebuilt tezuka bitstreams
(`data/vendor-vs-candidates.txt`):

| Candidate | used | common with vendor | same direction |
|---|---|---|---|
| tezuka **nano** | 48 | **48** | **48** |
| tezuka opensdrlab7010mini | 52 | 22 | 10 |
| tezuka fishball7010 | 61 | 29 | 15 |
| tezuka pciesdr7010 | 45 | 18 | 8 |
| kangyuzhe666 pluto-7010clg400 XDC | 52 | 25 | – |

Every pin tezuka drives on the Nano is driven the same way by the vendor firmware; the vendor uses
five more (L14, M14, M15, N15, N16: LEDs / clock control, to be confirmed). The tezuka nano device
tree also matches the vendor's: USB PHY reset on GPIO 46 (ADI uses 52), no ADM1177 current limiter.

## Repository layout

- `tools/` — bitstream frame reader, IOB decoder, pin comparison, database fetch script.
- `data/` — decoded vendor pad configuration and the candidate comparison. No bitstreams.
- `board/nano/dts/` — Nano device tree from tezuka_fw (GPL, see that repository).
- `firmware/` — `REQUIREMENTS.md` (lean image, timestamps, USB network drivers incl. aic8800d80) and the build recipe: ADI HDL + Quantulum timestamp IP + Nano constraints, in pgreenland's
  `plutosdr-fw` tree.
- `runner/` — self-hosted runner setup for the Vivado host.
- `.github/workflows/` — `firmware.yml` installs Vivado on the runner and builds the Nano bitstream + firmware (manual dispatch); `tools.yml` checks the Nano constraints against the decoded vendor pads and re-validates the decoder against the official ADI bitstream on
  every push.

## Recovering the firmware

Step-by-step flashing (RAM boot, DFU flash, serial console): [docs/FLASHING.md](docs/FLASHING.md).


Back up before flashing: `device_reboot sf` (or `dfu = 1` in `config.txt`), then
`dfu-util -a firmware.dfu -U vendor-firmware.dfu`, `-a boot.dfu -U ...`, `-a uboot-env.dfu -U ...`.
Restore with `dfu-util -a firmware.dfu -D vendor-firmware.dfu`. The vendor image is proprietary and is
not part of this repository.
