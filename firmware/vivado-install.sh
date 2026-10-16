#!/usr/bin/env bash
# Unattended Vivado 2022.2 ML Standard install with Zynq-7000 devices only, for a CI runner.
# usage: vivado-install.sh <Xilinx_Unified_2022.2_*_Lin64.bin> <destination e.g. /opt/Xilinx>
# Needs AMD_USER / AMD_PASS in the environment (the free edition still requires an account for
# the download). Prints the installed size; fails loudly on any step.
set -euo pipefail
BIN=${1:?installer}; DEST=${2:-/opt/Xilinx}
chmod +x "$BIN"; mkdir -p "$DEST" work-vivado && export TMPDIR=$PWD/work-vivado && cd work-vivado
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
# keep the installer's own module list (names differ between releases): everything off,
# then Zynq-7000 on. Product=Vivado already selects the design tools.
echo "--- generated Modules: $(grep '^Modules=' "$CFG")"
python3 - "$CFG" <<'PY'
import re, sys
p = sys.argv[1]; s = open(p).read()
m = re.search(r'^Modules=(.*)$', s, re.M); mods = m.group(1).split(',')
out = []
for mod in mods:
    name = mod.rsplit(':', 1)[0]
    out.append(f"{name}:{1 if name.strip() == 'Zynq-7000' else 0}")
assert any(x.endswith('Zynq-7000:1') for x in out), out
s = s[:m.start(1)] + ','.join(out) + s[m.end(1):]
open(p, 'w').write(s)
PY
sed -i 's/^EnableDiskUsageOptimization=.*/EnableDiskUsageOptimization=0/; s/^CreateProgramGroupShortcuts=.*/CreateProgramGroupShortcuts=0/; s/^CreateDesktopShortcuts=.*/CreateDesktopShortcuts=0/; s/^CreateFileAssociation=.*/CreateFileAssociation=0/' "$CFG"
echo "--- install_config.txt"; cat "$CFG"
# 3. install (downloads only the selected modules)
df -h "$DEST" "$HOME" /tmp
# the installer redraws a progress line ten times a second; keep one line per minute
set +e
./xsetup --agree XilinxEULA,3rdPartyEULA --batch Install --config "$CFG" 2>&1 | \
  awk '/Downloading files|Installing files|Extracting/ { t = systime(); if (t - last < 60) next; last = t } { print; fflush() }'
RC=${PIPESTATUS[0]}
set -e
if [ "$RC" -ne 0 ]; then
  echo "--- installer log (space check)"
  LOG=$(ls -t "$HOME"/.Xilinx/xinstall/*.log 2>/dev/null | head -1); echo "log: $LOG"
  grep -n -i -E "space|size|bytes|free|require|archive|module:|dest:" "$LOG" | tail -80
  exit 1
fi
du -sh "$DEST"; ls "$DEST"/Vivado
