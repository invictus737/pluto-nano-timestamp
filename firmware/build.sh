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
sed -i 's/xc7z010clg225-1/xc7z010clg400-1/' hdl/projects/pluto/system_project.tcl
cp "$XDC" hdl/projects/pluto/system_constr.xdc
# device tree: apply the Nano differences (kept as a patch next to this script once finalised)
[ -f "$HERE/nano-dt.patch" ] && git -C linux apply "$HERE/nano-dt.patch"
export TARGET=pluto
make -j"$(nproc)"
ls -la build/pluto.frm build/pluto.dfu
