#!/usr/bin/env bash
# TX timestamp gate regression (Icarus): the real util_upack2_timestamp with functional FIFO/CDC stubs
# on one clock. Every case must print PASS: no payload leaves before its timestamp, none is lost.
# Not a CDC/XPM, timing or bitstream check. usage: firmware/hdl-quantulum/tests/run.sh [work dir]
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); W=${1:-$(mktemp -d)}
iverilog -g2012 -s pluto_timestamp_bubble_test -o "$W/tb.vvp" "$HERE/upack2_timestamp_bubble_tb.v" \
  "$HERE/../util_upack2_timestamp/src/util_upack2_timestamp.v"
for c in bubble consecutive backpressure discard header_full_after_discard disable abort; do
  vvp "$W/tb.vvp" "+CASE=$c" | tee "$W/$c.log" | grep -q "PASS $c" || { echo "FAIL $c"; cat "$W/$c.log"; exit 1; }
  echo "PASS $c"
done
