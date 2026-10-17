#!/usr/bin/env bash
# ADI plutosdr-fw v0.39 with pgreenland's RX/TX timestamping ported on top (patches/v0.39 and the
# vendored hdl-quantulum IP repository). Replaces the pgreenland v0.38_timestamp superproject.
# usage: firmware/fetch-fw.sh <destination> [--hdl-only]
#   --hdl-only: bitstream job, only the hdl submodule (plus the timestamp IP repository).
set -euo pipefail
DEST=${1:?destination}; MODE=${2:-}; HERE=$(cd "$(dirname "$0")" && pwd)
FW_TAG=v0.39
FW_COMMIT=9e90bce43f849882cd43c20a08effca132790fb3   # analogdevicesinc/plutosdr-fw v0.39
PATCHES=$HERE/patches/$FW_TAG
if [ ! -d "$DEST/.git" ]; then
  git clone -q -b "$FW_TAG" https://github.com/analogdevicesinc/plutosdr-fw.git "$DEST"
fi
[ "$(git -C "$DEST" rev-parse HEAD)" = "$FW_COMMIT" ] || { echo "plutosdr-fw $FW_TAG is not $FW_COMMIT"; exit 1; }
if [ "$MODE" = --hdl-only ]; then SUBS=hdl; else SUBS="hdl linux buildroot u-boot-xlnx"; fi
git -C "$DEST" submodule update --init --depth 1 $SUBS
for sub in $SUBS; do
  [ -d "$PATCHES/$sub" ] || continue
  for p in "$PATCHES/$sub"/*.patch; do
    if git -C "$DEST/$sub" apply --reverse --check "$p" 2>/dev/null; then echo "already applied: $sub/$(basename "$p")"; continue; fi
    git -C "$DEST/$sub" apply --whitespace=nowarn "$p" && echo "applied: $sub/$(basename "$p")"
  done
done
# projects/pluto/system_bd.tcl adds ../../../hdl-quantulum (the superproject root) as an IP repository
rm -rf "$DEST/hdl-quantulum" && cp -R "$HERE/hdl-quantulum" "$DEST/hdl-quantulum"
git -C "$DEST" submodule status
