#!/bin/sh
# macOS: qa_run.sh inside the colima arm64 container (as run-colima.sh). usage: qa_run-colima.sh [args]
set -e
C=${CONTAINER:-mbneo}
cd "$(dirname "$0")/../.."
COPYFILE_DISABLE=1 tar --no-xattrs -cf - --exclude .git config port test CherryUSB/core CherryUSB/common \
  CherryUSB/class/hub CherryUSB/port/ehci CherryUSB/osal 2>/dev/null | \
  docker exec -i -e SEED="${SEED:-}" -e QA_SOAK="${QA_SOAK:-}" "$C" sh -c 'rm -rf /tmp/cu-qa && mkdir -p /tmp/cu-qa/out && cd /tmp/cu-qa && tar xf - && OUT=/tmp/cu-qa/out bash test/ehci_nxp/qa_run.sh "$@"' sh "$@"
