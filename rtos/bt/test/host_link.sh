#!/bin/sh
# Builds rtos/bt with BTstack for the host (fake board: test/host/fake_board.c) and runs the
# manager end to end: bt status/on/scan/devices/off against a scripted HCI controller.
# This checks the port compiles, links completely and drives BTstack; it is not the RTEMS
# build (arm-rtems6-gcc) and not a radio test.
# usage: host_link.sh <out dir>
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
BT=$(cd "$HERE/.." && pwd)
RTOS=$(cd "$BT/.." && pwd)
OUT=${1:?out dir}
BTSTACK=${BTSTACK_DIR:-$BT/btstack}
CC=${CC:-cc}
SRC=$(make -s -f "$BT/btstack.mk" btstack-print)
INC=$(make -s -f "$BT/btstack.mk" btstack-print-inc)
IFLAGS="-I$HERE/host -I$BT -I$RTOS/libbsd/aic8800"
for d in $INC; do IFLAGS="$IFLAGS -I$BTSTACK/$d"; done
SAN=${SAN:--fsanitize=address,undefined}
mkdir -p "$OUT/obj"
objs=
for f in $SRC; do
  o="$OUT/obj/$(echo "$f" | tr '/' '_').o"
  $CC -std=gnu11 -O1 -g -w $SAN $IFLAGS -c "$BTSTACK/$f" -o "$o"
  objs="$objs $o"
done
for f in nexus_bt.c bt_audio.c btstack_run_loop_rtems.c hci_transport_aicwf.c; do
  o="$OUT/obj/nexus_$f.o"
  $CC -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter $SAN $IFLAGS -c "$BT/$f" -o "$o"
  objs="$objs $o"
done
for f in fake_board.c host_main.c; do
  $CC -std=gnu11 -O1 -g -Wall $SAN $IFLAGS -c "$HERE/host/$f" -o "$OUT/obj/$f.o"
  objs="$objs $OUT/obj/$f.o"
done
$CC $SAN -o "$OUT/bt_host" $objs -lm -lpthread
echo "host link: ok ($(echo $SRC | wc -w | tr -d ' ') BTstack sources)"
mkdir -p "$OUT/data"
"$OUT/bt_host" "$OUT/data"
