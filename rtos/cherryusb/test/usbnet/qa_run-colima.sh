#!/bin/sh
# macOS: qa_run.sh inside the colima arm64 Linux container (as run-colima.sh does for run.sh).
# env: CONTAINER, SAN, CHERRY_SRC
C=${CONTAINER:-mbneo}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SRC=${CHERRY_SRC:-$ROOT/CherryUSB}
{
  COPYFILE_DISABLE=1 tar --no-xattrs -cf - -C "$ROOT" config net test/usbnet
  COPYFILE_DISABLE=1 tar --no-xattrs -cf - -C "$SRC" -s ',^,CherryUSB/,' common core class/hub class/cdc class/vendor/net
  COPYFILE_DISABLE=1 tar --no-xattrs -cf - -C "$ROOT/../libbsd" -s ',^,libbsd/,' aic8800/aicwf_usbif.h
} | docker exec -i -e SAN="${SAN:-address}" "$C" sh -c 'rm -rf /tmp/cu-qa && mkdir -p /tmp/cu-qa/out && cd /tmp/cu-qa && tar xif - && OUT=/tmp/cu-qa/out CHERRY_SRC=/tmp/cu-qa/CherryUSB AICWF_DIR=/tmp/cu-qa/libbsd/aic8800 bash test/usbnet/qa_run.sh "$@"' sh "$@"
