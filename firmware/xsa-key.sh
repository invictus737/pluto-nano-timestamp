#!/usr/bin/env bash
# Cache key of the Nano bitstream: Vivado version + every HDL input in this repository
# (the plutosdr-fw base is pinned by commit inside fetch-fw.sh, which is hashed too).
# usage: firmware/xsa-key.sh <vivado version>
set -euo pipefail
cd "$(dirname "$0")/.."
IN=$( { cat board/nano/system_constr.xdc board/nano/ps7.tcl firmware/apply-nano.sh firmware/build-hdl.sh firmware/fetch-fw.sh;
        find firmware/patches/v0.39/hdl firmware/hdl-quantulum -type f | LC_ALL=C sort | xargs cat; } | sha256sum | cut -c1-16)
echo "nano-xsa-v039-${1:?vivado version}-${IN}"
