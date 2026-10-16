#!/usr/bin/env bash
# Retarget a pgreenland plutosdr-fw (v0.38_timestamp) checkout to the HamGeek NanoSDR.
# usage: firmware/apply-nano.sh <path/to/plutosdr-fw>
# Edits are minimal and asserted: part, PS7 package/bank/USB-reset/DDR, constraints, device tree.
set -euo pipefail
FW=${1:?plutosdr-fw checkout}
HERE=$(cd "$(dirname "$0")" && pwd); BOARD=$HERE/../board/nano
P=$FW/hdl/projects/pluto
need() { grep -q -- "$2" "$1" || { echo "expected '$2' in $1"; exit 1; }; }
# 1. FPGA part
need "$P/system_project.tcl" 'xc7z010clg225-1'
sed -i 's/xc7z010clg225-1/xc7z010clg400-1/' "$P/system_project.tcl"
# 2. PS7: package, bank 0 voltage, USB0 reset MIO, DDR timing of the Nano (F5OEO nano/ps7.tcl)
B=$P/system_bd.tcl
need "$B" 'CONFIG.PCW_PACKAGE_NAME clg225'
need "$B" 'CONFIG.PCW_PRESET_BANK0_VOLTAGE {LVCMOS 1.8V}'
need "$B" 'CONFIG.PCW_USB0_RESET_IO {MIO 52}'
sed -i 's/CONFIG.PCW_PACKAGE_NAME clg225/CONFIG.PCW_PACKAGE_NAME clg400/' "$B"
sed -i 's/CONFIG.PCW_PRESET_BANK0_VOLTAGE {LVCMOS 1.8V}/CONFIG.PCW_PRESET_BANK0_VOLTAGE {LVCMOS 3.3V}/' "$B"
sed -i 's/CONFIG.PCW_USB0_RESET_IO {MIO 52}/CONFIG.PCW_USB0_RESET_IO {MIO 46}/' "$B"
grep -q 'PCW_UIPARAM_DDR_FREQ_MHZ' "$B" || sed -i '/PCW_UIPARAM_DDR_BOARD_DELAY1 0.240/a\
ad_ip_parameter sys_ps7 CONFIG.PCW_UIPARAM_DDR_FREQ_MHZ 400.000000\
ad_ip_parameter sys_ps7 CONFIG.PCW_UIPARAM_DDR_DQS_TO_CLK_DELAY_0 0.025' "$B"
# 3. Constraints
cp "$BOARD/system_constr.xdc" "$P/system_constr.xdc"
# 4. Device tree: Nano differences in the three Pluto DTs the FIT carries
for dts in "$FW"/linux/arch/arm/boot/dts/zynq-pluto-sdr.dtsi; do
  [ -f "$dts" ] || { echo "missing $dts"; exit 1; }
  need "$dts" 'xlnx,phy-reset-gpio = <&gpio0 52 0>'
  sed -i 's/xlnx,phy-reset-gpio = <&gpio0 52 0>/xlnx,phy-reset-gpio = <\&gpio0 46 0>/' "$dts"
done
# remove the ADM1177 current-limiter node (the Nano has none): exact brace-matched node removal
python3 - "$FW"/linux/arch/arm/boot/dts/zynq-pluto-sdr*.dts <<'PY'
import sys, re
for path in sys.argv[1:]:
    s = open(path).read()
    m = re.search(r'\n[ \t]*current_limiter@5a\s*\{', s)
    if not m: continue
    i = s.index('{', m.start()); depth = 0
    for j in range(i, len(s)):
        depth += {'{': 1, '}': -1}.get(s[j], 0)
        if depth == 0: break
    end = s.index(';', j) + 1
    s = s[:m.start()] + s[end:]
    open(path, 'w').write(s); print('removed current_limiter@5a from', path)
PY
echo "nano retarget applied to $FW"
grep -n 'clg400\|BANK0_VOLTAGE\|USB0_RESET_IO\|DDR_FREQ' "$B" | head -5
