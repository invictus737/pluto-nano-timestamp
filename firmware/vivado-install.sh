#!/usr/bin/env bash
# Unattended Vivado 2022.2 ML Standard install with Zynq-7000 devices only, for a CI runner.
# usage: vivado-install.sh <Xilinx_Unified_2022.2_*_Lin64.bin> <destination e.g. /opt/Xilinx>
# Needs AMD_USER / AMD_PASS in the environment (the free edition still requires an account for
# the download). Prints the installed size; fails loudly on any step.
set -euo pipefail
BIN=${1:?installer}; DEST=${2:-/opt/Xilinx}
chmod +x "$BIN"; mkdir -p "$DEST" work-vivado && cd work-vivado
"../$BIN" --keep --noexec --target xsetup >/dev/null
cd xsetup
: "${AMD_USER:?}" "${AMD_PASS:?}"
# 1. authentication token (interactive prompt driven by expect)
expect <<EXP
set timeout 120
spawn ./xsetup -b AuthTokenGen
expect -re "(?i)user ?id|e-?mail" { send "$AMD_USER\r" }
expect -re "(?i)password" { send "$AMD_PASS\r" }
expect eof
EXP
# 2. configuration: Vivado only, Zynq-7000 only, no shortcuts, no cable drivers
./xsetup -b ConfigGen <<< $'2\n1\n' || true   # 2 = Vivado, 1 = ML Standard (installer menus)
CFG=$(ls ~/.Xilinx/install_config.txt)
sed -i "s|^Destination=.*|Destination=$DEST|" "$CFG"
sed -i 's/^Modules=.*/Modules=Zynq-7000:1,Vivado Design Suite:1,Vitis Model Composer (Simulink):0,Zynq UltraScale+ MPSoC:0,Zynq UltraScale+ RFSoC:0,Virtex UltraScale+:0,Kintex UltraScale+:0,Kintex UltraScale:0,Virtex UltraScale:0,Versal:0,Artix UltraScale+:0,Artix-7:0,Kintex-7:0,Virtex-7:0,Spartan-7:0,Install Cable Drivers:0,DocNav:0,Engineering Sample Devices:0/' "$CFG"
sed -i 's/^CreateProgramGroupShortcuts=.*/CreateProgramGroupShortcuts=0/; s/^CreateDesktopShortcuts=.*/CreateDesktopShortcuts=0/; s/^CreateFileAssociation=.*/CreateFileAssociation=0/' "$CFG"
echo "--- install_config.txt"; cat "$CFG"
# 3. install (downloads only the selected modules)
./xsetup --agree XilinxEULA,3rdPartyEULA --batch Install --config "$CFG"
du -sh "$DEST"; ls "$DEST"/Vivado
