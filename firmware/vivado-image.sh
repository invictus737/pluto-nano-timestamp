#!/usr/bin/env bash
# Installed Vivado as a zstd tar stream kept in the private release "vivado-<VIVADO_VERSION>-zynq7-image"
# (split in <2 GB parts). Runs unpack it instead of reinstalling.
# usage: vivado-image.sh fetch <mountpoint>   -> exit 0 and Vivado mounted, or exit 3 if no image yet
#        vivado-image.sh publish <install-dir> -> pack, split, upload (needs GH_TOKEN with contents:write)
set -euo pipefail
export GH_REPO=${GH_REPO:-${GITHUB_REPOSITORY:-invictus737/pluto-nano-timestamp}}  # gh runs outside the checkout
VIVADO_VERSION=${VIVADO_VERSION:-2022.2}
TAG=vivado-$VIVADO_VERSION-zynq7-image
CMD=${1:?fetch|publish}; DIR=${2:?dir}
WORK=${WORK:-/mnt/build/work/vimg}; mkdir -p "$WORK"
case "$CMD" in
fetch)
  # streamed: each part is downloaded to stdout and unpacked on the fly, never stored whole
  gh release view "$TAG" >/dev/null 2>&1 || { echo "no Vivado image release yet"; exit 3; }
  PARTS=$(gh release view "$TAG" --json assets -q '.assets[].name' | { grep '^vivado.tar.zst.part' || true; } | sort)
  [ -n "$PARTS" ] || { echo "Vivado image release has no parts"; exit 3; }
  gh release view "$TAG" --json assets -q '.assets[].name' | grep -qx 'vivado.complete' || { echo "Vivado image upload incomplete"; exit 3; }
  mkdir -p "$DIR"
  for p in $PARTS; do gh release download "$TAG" --pattern "$p" --output -; done | zstd -d -T0 | tar -C "$DIR" -xf -
  ls "$DIR"; du -sh "$DIR"
  ;;
publish)
  # streamed: tar | zstd | split, each 1.9 GB part uploaded and deleted as soon as it is written,
  # so publishing needs about 2 GB free instead of the whole image
  sudo apt-get install -y -q zstd >/dev/null
  gh release view "$TAG" >/dev/null 2>&1 || gh release create "$TAG" --title "Vivado $VIVADO_VERSION (Zynq-7000 only), installed, tar.zst parts" --notes "Private CI asset: installed AMD Vivado ML Standard $VIVADO_VERSION, Zynq-7000 devices only, unpacked by the firmware workflow. Not redistributed."
  cd "$WORK"
  export TAG
  tar -C "$DIR" -cf - . | zstd -T0 -10 | split -b 1900M -d -a 2 \
    --filter='cat > "$FILE" && gh release upload "$TAG" "$FILE" --clobber && rm -f "$FILE"' - vivado.tar.zst.part
  echo "complete $(date -u +%FT%TZ)" > vivado.complete && gh release upload "$TAG" vivado.complete --clobber  # GitHub rejects empty assets
  gh release view "$TAG" --json assets -q '.assets[] | .name+" "+(.size|tostring)'
  ;;
esac
