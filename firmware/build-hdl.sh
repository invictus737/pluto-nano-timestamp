#!/usr/bin/env bash
# Build the Nano timestamping bitstream: pgreenland plutosdr-fw v0.38_timestamp HDL retargeted.
# usage: build-hdl.sh <plutosdr-fw checkout> ; needs vivado on PATH (or VIVADO_SETTINGS).
set -euo pipefail
FW=${1:?plutosdr-fw checkout}; HERE=$(cd "$(dirname "$0")" && pwd)
"$HERE/apply-nano.sh" "$FW" --hdl-only
[ -n "${VIVADO_SETTINGS:-}" ] && source "$VIVADO_SETTINGS"
vivado -version | head -1
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
mkdir -p "$FW/build" && cp projects/pluto/pluto.sdk/system_top.xsa "$FW/build/"
grep -i -E "critical warning|error" projects/pluto/pluto.runs/impl_1/*.log 2>/dev/null | grep -v "WARNING: \[" | head -20 || true
