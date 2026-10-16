#!/usr/bin/env bash
# Build the Nano timestamping bitstream: pgreenland plutosdr-fw v0.38_timestamp HDL retargeted.
# usage: build-hdl.sh <plutosdr-fw checkout> ; needs vivado on PATH (or VIVADO_SETTINGS).
set -euo pipefail
FW=${1:?plutosdr-fw checkout}; HERE=$(cd "$(dirname "$0")" && pwd)
"$HERE/apply-nano.sh" "$FW"
[ -n "${VIVADO_SETTINGS:-}" ] && source "$VIVADO_SETTINGS"
vivado -version | head -1
unset MAKEFLAGS
cd "$FW/hdl" && make -C projects/pluto ADI_MAX_OOC_JOBS="$(nproc)" 2>&1 | tail -40
ls -la projects/pluto/pluto.sdk/system_top.xsa
mkdir -p "$FW/build" && cp projects/pluto/pluto.sdk/system_top.xsa "$FW/build/"
grep -i -E "critical warning|error" projects/pluto/pluto.runs/impl_1/*.log 2>/dev/null | grep -v "WARNING: \[" | head -20 || true
