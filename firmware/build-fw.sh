#!/usr/bin/env bash
# Assemble pluto.frm / pluto.dfu from a prebuilt Nano system_top.xsa (no Vivado needed here).
# usage: build-fw.sh <plutosdr-fw checkout> <system_top.xsa>
set -euo pipefail
FW=${1:?plutosdr-fw checkout}; XSA=${2:?system_top.xsa}; HERE=$(cd "$(dirname "$0")" && pwd)
"$HERE/apply-nano.sh" "$FW"
mkdir -p "$FW/build" && cp "$XSA" "$FW/build/system_top.xsa"
cd "$FW" && export TARGET=pluto VIVADO_SETTINGS=/nonexistent
make build/pluto.frm build/pluto.dfu 2>&1 | tail -30
ls -la build/pluto.frm build/pluto.dfu; sha256sum build/pluto.frm build/pluto.dfu
