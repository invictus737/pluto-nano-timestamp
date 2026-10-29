#!/bin/sh
# Host tests for rtos/bt: the audio ring/resampler, then a compile + link check of the whole
# Bluetooth component (BTstack, run loop, transport, manager) against a fake aicwf_bt and
# config store. Needs rtos/bt/fetch-btstack.sh for the second part.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${TMPDIR:-/tmp}/nexus-bt-test.$$
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
CC=${CC:-cc}
$CC -std=gnu11 -O2 -Wall -Wextra -fsanitize=address,undefined -o "$OUT/bt_audio_test" \
  "$HERE/bt_audio_test.c" "$HERE/../bt_audio.c" -lm
"$OUT/bt_audio_test"
if [ -f "$HERE/../btstack/src/hci.c" ]; then
  sh "$HERE/host_link.sh" "$OUT"
else
  echo "host link check skipped: run rtos/bt/fetch-btstack.sh first"
fi
