#!/usr/bin/env bash
# Fetches the pinned CherryUSB release used by the USB_STACK=cherry image (not vendored): only
# the host parts that rtos/cherryusb/cherryusb.mk compiles (core, common, hub class, the MSC
# headers, the EHCI port) plus the OS abstraction sources for reference and the license,
# verified against the pinned commit, then applies rtos/cherryusb/patches/*.patch.
# Idempotent: an existing checkout at the pinned commit with the same patch set is kept.
# usage: rtos/cherryusb/fetch-cherryusb.sh [dest]   (default: rtos/cherryusb/CherryUSB)
# CherryUSB license: Apache-2.0 (<dest>/LICENSE).
set -euo pipefail
TAG=v1.6.1
COMMIT=c9625ffa773ad10b8824d1b5361bca2ccc1f3d1e
URL=${CHERRYUSB_URL:-https://github.com/cherry-embedded/CherryUSB}
HERE=$(cd "$(dirname "$0")" && pwd)
DEST=${1:-$HERE/CherryUSB}
PATCHES=$(ls "$HERE"/patches/*.patch 2>/dev/null | sort || true)
STAMP="$COMMIT $(cat $PATCHES /dev/null | sha256sum | cut -c1-16)"

if [ -f "$DEST/.nexus-cherryusb" ] && [ "$(cat "$DEST/.nexus-cherryusb")" = "$STAMP" ]; then
  echo "cherryusb: $TAG ($COMMIT) already at $DEST"
  exit 0
fi
rm -rf "$DEST"
mkdir -p "$DEST"
git -C "$DEST" init -q
git -C "$DEST" remote add origin "$URL"
git -C "$DEST" config core.sparseCheckout true
printf '%s\n' /core/ /common/ /class/hub/ /class/msc/ /port/ehci/ /osal/ /LICENSE /README.md \
  > "$DEST/.git/info/sparse-checkout"
git -C "$DEST" fetch -q --depth 1 --filter=blob:none origin "refs/tags/$TAG"
got=$(git -C "$DEST" rev-parse 'FETCH_HEAD^{commit}')
if [ "$got" != "$COMMIT" ]; then
  echo "cherryusb: tag $TAG is $got, expected $COMMIT: refusing" >&2
  exit 1
fi
git -C "$DEST" -c advice.detachedHead=false checkout -q "$COMMIT"
for p in $PATCHES; do
  git -C "$DEST" apply --whitespace=nowarn "$p"
  echo "cherryusb: applied $(basename "$p")"
done
echo "$STAMP" > "$DEST/.nexus-cherryusb"
echo "cherryusb: $TAG ($COMMIT) at $DEST ($(du -sh "$DEST" | cut -f1))"
