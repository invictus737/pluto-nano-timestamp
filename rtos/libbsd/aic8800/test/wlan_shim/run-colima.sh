#!/bin/sh
# macOS: run.sh inside the colima arm64 Linux container (default name mbneo); the sources are
# piped in with tar, nothing is mounted or written back. env: CONTAINER, SAN
set -e
C=${CONTAINER:-mbneo}
cd "$(dirname "$0")/../.."
COPYFILE_DISABLE=1 tar --no-xattrs -cf - shim rtsmart/aic8800 rtsmart/wlan aicwf_usbif.h aicwf_wifi.h test/wlan_shim 2>/dev/null | \
  docker exec -i -e SAN="${SAN:-address}" "$C" sh -c 'rm -rf /tmp/aic-shim && mkdir -p /tmp/aic-shim/out && cd /tmp/aic-shim && tar xf - && OUT=/tmp/aic-shim/out bash test/wlan_shim/run.sh "$@"' sh "$@"
