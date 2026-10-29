#!/bin/sh
# macOS: run run.sh inside the colima arm64 Linux container (default name mbneo); the tree is
# piped in with tar, nothing is mounted or written back. usage: run-colima.sh [run.sh args]
set -e
C=${CONTAINER:-mbneo}
cd "$(dirname "$0")/../.."
COPYFILE_DISABLE=1 tar --no-xattrs -cf - --exclude .git config port test CherryUSB/core CherryUSB/common \
  CherryUSB/class/hub CherryUSB/port/ehci CherryUSB/osal 2>/dev/null | \
  docker exec -i -e SEED="${SEED:-}" "$C" sh -c 'rm -rf /tmp/cu-nxp && mkdir -p /tmp/cu-nxp/out && cd /tmp/cu-nxp && tar xf - && OUT=/tmp/cu-nxp/out bash test/ehci_nxp/run.sh "$@"' sh "$@"
