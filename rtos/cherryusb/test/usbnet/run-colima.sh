#!/bin/sh
# macOS: run run.sh inside the colima arm64 Linux container (default name mbneo); the sources
# are piped in with tar, nothing is mounted or written back. usage: run-colima.sh [run.sh args]
# env: CONTAINER, SAN, CHERRY_SRC (a fetched CherryUSB outside rtos/cherryusb/CherryUSB)
set -e
C=${CONTAINER:-mbneo}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SRC=${CHERRY_SRC:-$ROOT/CherryUSB}
{
  COPYFILE_DISABLE=1 tar --no-xattrs -cf - -C "$ROOT" config net test/usbnet
  COPYFILE_DISABLE=1 tar --no-xattrs -cf - -C "$SRC" -s ',^,CherryUSB/,' common core class/hub class/cdc class/vendor/net
  COPYFILE_DISABLE=1 tar --no-xattrs -cf - -C "$ROOT/../libbsd" -s ',^,libbsd/,' aic8800/aicwf_usbif.h
} | docker exec -i -e SAN="${SAN:-address}" "$C" sh -c 'rm -rf /tmp/cu-net && mkdir -p /tmp/cu-net/out && cd /tmp/cu-net && tar xif - && OUT=/tmp/cu-net/out CHERRY_SRC=/tmp/cu-net/CherryUSB AICWF_DIR=/tmp/cu-net/libbsd/aic8800 bash test/usbnet/run.sh "$@"' sh "$@"
