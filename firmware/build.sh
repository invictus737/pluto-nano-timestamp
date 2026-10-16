#!/usr/bin/env bash
# Build the Nano timestamping firmware on a host with Vivado 2022.2 (settings64.sh sourced)
# and the ADI plutosdr-fw toolchain prerequisites. Not runnable on GitHub-hosted runners.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$HERE/work}
XDC=$HERE/../board/nano/system_constr.xdc
[ -f "$XDC" ] || { echo "missing $XDC: the Nano constraint file is not finished yet"; exit 2; }
mkdir -p "$WORK" && cd "$WORK"
[ -d plutosdr-fw ] || git clone --recursive -b v0.38_timestamp https://github.com/pgreenland/plutosdr-fw.git
cd plutosdr-fw
git -C hdl status --short | grep -q . || "$HERE/apply-nano.sh" "$PWD"
export TARGET=pluto
make -j"$(nproc)"
ls -la build/pluto.frm build/pluto.dfu
