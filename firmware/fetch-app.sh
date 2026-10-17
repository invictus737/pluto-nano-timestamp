#!/usr/bin/env bash
# Optional application for the image: release "app" of this repository, asset <name>-armv7.tar
# (a directory <name>/ with app.conf + binaries, prepared by the application's own project).
# Unpacked into the rootfs overlay under /opt. No release or asset: the image has no application.
# usage: fetch-app.sh <rootfs-overlay dir>
set -euo pipefail
OVL=$(mkdir -p "${1:?overlay dir}" && cd "$1" && pwd); TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
if ! gh release download app -p '*-armv7.tar' -D "$TMP" 2>/dev/null; then echo "no application release: image without application"; exit 0; fi
mkdir -p "$OVL/opt"
for t in "$TMP"/*-armv7.tar; do tar -C "$OVL/opt" -xf "$t"; echo "application: $(basename "$t")"; done
find "$OVL/opt" -maxdepth 2 -name app.conf -exec sh -c 'echo "== $1"; cat "$1"' _ {} \;
du -sh "$OVL/opt"/*
