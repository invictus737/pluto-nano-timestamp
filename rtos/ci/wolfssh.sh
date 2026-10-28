#!/usr/bin/env bash
# wolfSSL (wolfCrypt only) + wolfSSH static libraries for arm-rtems6, for the SSH server (M2e).
# Configuration: rtos/ssh/user_settings.h (WOLFSSL_USER_SETTINGS), shared with rtos/app/Makefile.
# Pinned release tarballs, checked by SHA-256; the sources are compiled directly (no autotools)
# with the BSP flags from pkg-config, like rtos/app/Makefile, and archived deterministically.
# usage: rtos/ci/wolfssh.sh <toolchain prefix> <bsp prefix> <install prefix>
set -euo pipefail
TOOLS=${1:?toolchain prefix}; PREFIX=${2:?bsp prefix}; DEST=${3:?install prefix}
HERE=$(cd "$(dirname "$0")/.." && pwd); WORK=${WORK:-$PWD/work-wolf}; BSP=xilinx_zynq_zedboard

WOLFSSL_VER=5.9.1
WOLFSSL_SHA256=d5ca7af48cd2d9a91d539e9baedeba55a0605a28d7ac8b01dc3d5254a13ca341
WOLFSSH_VER=1.5.0
WOLFSSH_SHA256=5d9be7a577c65b725f1f30ebf2197d7caedd2c9c0012aa113e78a3396104838f

# wolfCrypt objects needed by wolfSSH with this configuration (disabled algorithms compile
# to empty objects, so a few extra names cost nothing).
CRYPT="aes asn coding cpuid curve25519 ecc ed25519 error fe_operations ge_operations hash hmac
       kdf logging memory random sha sha256 sha512 signature sp_c32 sp_int wc_encrypt
       wc_port wolfmath"
SSH="agent certman internal io keygen log port ssh wolfscp wolfsftp wolfterm"

export PATH="$TOOLS/bin:$PATH"
CC=arm-rtems6-gcc; AR=arm-rtems6-ar; NM=arm-rtems6-nm
PKG="$PREFIX/lib/pkgconfig/arm-rtems6-$BSP.pc"
BSPFLAGS=$(PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" pkg-config --cflags "$PKG")
mkdir -p "$WORK" && cd "$WORK"

fetch() { # name version sha256
  local f=$1-$2.tar.gz
  [ -f "$f" ] || curl -sfL -o "$f" "https://github.com/wolfSSL/$1/archive/refs/tags/v$2-stable.tar.gz"
  echo "$3  $f" | sha256sum -c -
  rm -rf "$1-$2-stable"; tar xzf "$f"
}
fetch wolfssl $WOLFSSL_VER $WOLFSSL_SHA256
fetch wolfssh $WOLFSSH_VER $WOLFSSH_SHA256
SSL=$WORK/wolfssl-$WOLFSSL_VER-stable; SSHSRC=$WORK/wolfssh-$WOLFSSH_VER-stable

# relative __FILE__ paths keep the archives independent of the work directory
CFLAGS="$BSPFLAGS -O2 -g -ffunction-sections -fdata-sections -fdebug-prefix-map=$WORK=. \
  -DWOLFSSL_USER_SETTINGS -I$HERE/ssh -I$SSL -I$SSHSRC -Wall -Wno-unused-function"
rm -rf obj && mkdir -p obj/ssl obj/ssh
build() { # src obj
  echo "  CC $(basename "$1")"
  # shellcheck disable=SC2086
  $CC $CFLAGS -c "$1" -o "$2"
}
export -f build; export CC CFLAGS
for s in $CRYPT; do echo "$SSL/wolfcrypt/src/$s.c obj/ssl/$s.o"; done | xargs -P"$(nproc)" -n2 bash -c 'build "$0" "$1"'
for s in $SSH; do echo "$SSHSRC/src/$s.c obj/ssh/$s.o"; done | xargs -P"$(nproc)" -n2 bash -c 'build "$0" "$1"'

# stage first: the install prefix only ever holds a complete, checked set (rtos/app/Makefile
# enables the SSH server when $DEST/lib/libwolfssh.a exists)
STAGE=$WORK/stage; rm -rf "$STAGE" && mkdir -p "$STAGE/lib" "$STAGE/include"
(cd obj/ssl && $AR rcsD "$STAGE/lib/libwolfssl.a" $(for s in $CRYPT; do echo $s.o; done))
(cd obj/ssh && $AR rcsD "$STAGE/lib/libwolfssh.a" $(for s in $SSH; do echo $s.o; done))
cp -r "$SSL/wolfssl" "$SSHSRC/wolfssh" "$STAGE/include/"
{
  echo "wolfssl $WOLFSSL_VER $WOLFSSL_SHA256"
  echo "wolfssh $WOLFSSH_VER $WOLFSSH_SHA256"
  echo "user_settings.h $(sha256sum "$HERE/ssh/user_settings.h" | cut -d' ' -f1)"
} > "$STAGE/BUILDINFO"

# the configuration must have produced the pieces the server uses
SYMS=$($NM -g --defined-only "$STAGE/lib/libwolfssh.a" "$STAGE/lib/libwolfssl.a")
for sym in wolfSSH_accept wolfSSH_SFTP_read wolfSSH_SetTerminalResizeCb wc_ed25519_sign_msg \
           wc_curve25519_shared_secret wc_AesGcmEncrypt wc_ecc_verify_hash wc_InitRng; do
  grep -qw "$sym" <<< "$SYMS" || { echo "wolf libraries lack $sym"; exit 1; }
done
mkdir -p "$DEST" && find "$DEST" -mindepth 1 -delete && cp -r "$STAGE"/. "$DEST"/
cat "$DEST/BUILDINFO"; ls -la "$DEST/lib"
