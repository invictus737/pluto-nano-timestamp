#!/bin/sh
# Host unit tests of the configuration parser/merge/mask/U-Boot env code (rtos/app/config_kv.c).
# usage: rtos/config/test/run.sh [uboot-env.bin]   (optional: a 128 KiB dump of the env partition,
# e.g. a dfu_sf upload of uboot-env.dfu; only read)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${TMPDIR:-/tmp}/nexus-config-test.$$
CC=${CC:-cc}
$CC -std=c99 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra -Werror -g -fsanitize=address,undefined \
	-o "$OUT" "$HERE/test_config_kv.c" "$HERE/../../app/config_kv.c"
set +e
"$OUT" "$HERE/../config.txt.example" "$@"
rc=$?
rm -rf "$OUT" "$OUT.dSYM"
exit $rc
