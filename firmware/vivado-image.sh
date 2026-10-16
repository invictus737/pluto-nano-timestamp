#!/usr/bin/env bash
# Installed Vivado as a compressed, read-only SquashFS image kept in the private release
# "vivado-2022.2-zynq7-image" (split in <2 GB parts). Runs mount it instead of reinstalling.
# usage: vivado-image.sh fetch <mountpoint>   -> exit 0 and Vivado mounted, or exit 3 if no image yet
#        vivado-image.sh publish <install-dir> -> pack, split, upload (needs GH_TOKEN with contents:write)
set -euo pipefail
TAG=vivado-2022.2-zynq7-image
CMD=${1:?fetch|publish}; DIR=${2:?dir}
WORK=${WORK:-/mnt/build/work/vimg}; mkdir -p "$WORK"
case "$CMD" in
fetch)
  gh release view "$TAG" >/dev/null 2>&1 || { echo "no Vivado image release yet"; exit 3; }
  gh release download "$TAG" --pattern 'vivado.sqfs.part*' --dir "$WORK" --clobber
  cat $(ls "$WORK"/vivado.sqfs.part* | sort) > "$WORK/vivado.sqfs" && rm -f "$WORK"/vivado.sqfs.part*
  ( cd "$WORK" && gh release download "$TAG" --pattern 'vivado.sqfs.sha256' --clobber && sha256sum -c vivado.sqfs.sha256 )
  sudo mkdir -p "$DIR" && sudo mount -t squashfs -o loop,ro "$WORK/vivado.sqfs" "$DIR"
  ls "$DIR"; du -sh "$WORK/vivado.sqfs"
  ;;
publish)
  sudo apt-get install -y -q squashfs-tools >/dev/null
  mksquashfs "$DIR" "$WORK/vivado.sqfs" -comp zstd -Xcompression-level 15 -b 1M -no-progress -quiet -processors "$(nproc)"
  du -sh "$DIR" "$WORK/vivado.sqfs"
  ( cd "$WORK" && sha256sum vivado.sqfs > vivado.sqfs.sha256 && split -b 1900M -d -a 2 vivado.sqfs vivado.sqfs.part && rm vivado.sqfs )
  gh release view "$TAG" >/dev/null 2>&1 || gh release create "$TAG" --title "Vivado 2022.2 (Zynq-7000 only), installed, SquashFS zstd" --notes "Private CI asset: installed AMD Vivado ML Standard 2022.2, Zynq-7000 devices only, mounted read-only by the firmware workflow. Not redistributed."
  gh release upload "$TAG" "$WORK"/vivado.sqfs.part* "$WORK/vivado.sqfs.sha256" --clobber
  ;;
esac
