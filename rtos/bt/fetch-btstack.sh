#!/usr/bin/env bash
# Fetches the pinned BTstack release used by rtos/bt (not vendored): only the parts the RTEMS
# port compiles (src, platform/posix, the bluedroid SBC encoder) plus the license, verified
# against the pinned commit. Idempotent: an existing checkout at the pinned commit is kept.
# usage: rtos/bt/fetch-btstack.sh [dest]   (default: rtos/bt/btstack, which bt.mk uses)
# BTstack license: BlueKitchen GmbH BSD-style with a non-commercial clause (commercial use needs
# a BlueKitchen license); accepted for this project by its owner. See <dest>/LICENSE.
set -euo pipefail
TAG=v1.8.3
COMMIT=51e11ce3692abfe31b4ee3d66cffdeb1ae1626f2
URL=${BTSTACK_URL:-https://github.com/bluekitchen/btstack}
DEST=${1:-$(cd "$(dirname "$0")" && pwd)/btstack}

if [ -f "$DEST/.nexus-btstack" ] && [ "$(cat "$DEST/.nexus-btstack")" = "$COMMIT" ]; then
  echo "btstack: $TAG ($COMMIT) already at $DEST"
  exit 0
fi
rm -rf "$DEST"
mkdir -p "$DEST"
git -C "$DEST" init -q
git -C "$DEST" remote add origin "$URL"
git -C "$DEST" config core.sparseCheckout true
printf '%s\n' /src/ /platform/posix/ /3rd-party/bluedroid/encoder/ /3rd-party/bluedroid/decoder/ \
  /LICENSE /README.md > "$DEST/.git/info/sparse-checkout"
git -C "$DEST" fetch -q --depth 1 --filter=blob:none origin "refs/tags/$TAG"
got=$(git -C "$DEST" rev-parse 'FETCH_HEAD^{commit}')
if [ "$got" != "$COMMIT" ]; then
  echo "btstack: tag $TAG is $got, expected $COMMIT: refusing" >&2
  exit 1
fi
git -C "$DEST" -c advice.detachedHead=false checkout -q "$COMMIT"
echo "$COMMIT" > "$DEST/.nexus-btstack"
echo "btstack: $TAG ($COMMIT) at $DEST ($(du -sh "$DEST" | cut -f1))"
