#!/usr/bin/env bash
# Build the Nano timestamping bitstream: ADI plutosdr-fw v0.39 HDL + timestamping (firmware/fetch-fw.sh),
# retargeted to the Nano. usage: build-hdl.sh <plutosdr-fw checkout> ; needs vivado on PATH (or VIVADO_SETTINGS).
set -euo pipefail
FW=${1:?plutosdr-fw checkout}; HERE=$(cd "$(dirname "$0")" && pwd)
"$HERE/apply-nano.sh" "$FW" --hdl-only
[ -n "${VIVADO_SETTINGS:-}" ] && source "$VIVADO_SETTINGS"
VER=$(vivado -version | head -1); echo "$VER"
# ADI hdl of plutosdr-fw v0.39 is released for Vivado 2023.2; another version builds only with the
# version check disabled (reported as a critical warning in the Vivado log).
case "$VER" in *v2023.2*) ;; *) echo "WARNING: not Vivado 2023.2, ADI version check disabled"; export ADI_IGNORE_VERSION_CHECK=1 ;; esac
[ -d "$FW/hdl-quantulum" ] || { echo "missing $FW/hdl-quantulum (run firmware/fetch-fw.sh)"; exit 1; }
( cd "$FW/hdl-quantulum" && vivado -mode batch -nojournal -nolog -source refresh.tcl -tclargs "$FW/hdl-quantulum" | grep -E "refreshed|ERROR" ) \
  || echo "WARNING: IP checksum refresh failed, continuing with the edited component.xml"
unset MAKEFLAGS
# ADI_MAX_OOC_JOBS must be an environment variable: as a make argument the ADI makefiles take it
# for a project parameter and build in projects/pluto/ADIMAXOOCJOBS2/ instead.
export ADI_MAX_OOC_JOBS="$(nproc)"
cd "$FW/hdl"
if ! make -C projects/pluto 2>&1 | tail -40; then :; fi
if [ ! -f projects/pluto/pluto.sdk/system_top.xsa ]; then
  echo "--- bitstream build failed; end of the Vivado logs"
  for log in projects/pluto/pluto_vivado.log projects/pluto/pluto.runs/*/runme.log; do
    [ -f "$log" ] && { echo "=== $log"; grep -n -E "ERROR|CRITICAL WARNING" "$log" | head -40; tail -30 "$log"; }
  done
  exit 1
fi
ls -la projects/pluto/pluto.sdk/system_top.xsa
# the timestamp cores must be in the implemented design
N=$(unzip -p projects/pluto/pluto.sdk/system_top.xsa '*.hwh' | grep -o -E 'MODTYPE="util_(cpack2_timestamp|upack2_timestamp|wr_sync_mux)"' | sort | uniq -c)
echo "$N"; [ "$(echo "$N" | grep -c MODTYPE)" -eq 3 ] || { echo "timestamp IP missing from system_top.xsa"; exit 1; }
mkdir -p "$FW/build" && cp projects/pluto/pluto.sdk/system_top.xsa "$FW/build/"
grep -i -E "critical warning|error" projects/pluto/pluto.runs/impl_1/*.log 2>/dev/null | grep -v "WARNING: \[" | head -20 || true
