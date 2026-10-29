#!/usr/bin/env bash
# Rust toolchain for nexus2 on RTEMS: a linked copy of a pinned nightly whose standard library
# sources build against the corrected libc crate in the nexus2 tree (vendor/libc, RTEMS 6.2 +
# libbsd layouts). -Zbuild-std builds std from the toolchain's own rust-src workspace and ignores
# the [patch] of the project being built, so the patch goes into that workspace instead.
# usage: rtos/ci/rust-toolchain.sh <base toolchain> <libc crate dir> <new toolchain name>
# prints nothing on success but the toolchain name; RUST_TOOLCHAIN=<name> then selects it.
set -euo pipefail
BASE=${1:?base toolchain, e.g. nightly-2025-07-01}; LIBC=$(cd "${2:?libc crate dir}" && pwd); NAME=${3:?toolchain name}
DEST=${RUST_LIBC_TOOLCHAIN_DIR:-$HOME/.rustup/linked}/$NAME
LIB=lib/rustlib/src/rust/library
# idempotent: an existing linked copy patched to this same libc directory is reused (a cache
# may restore $DEST; the link itself is re-registered every time)
THREAD_RS=std/src/sys/pal/unix/thread.rs
NAME_MARK="nexus-rtems: thread names"
if [ -f "$DEST/$LIB/Cargo.toml" ] && grep -qF "libc = { path = \"$LIBC\" }" "$DEST/$LIB/Cargo.toml" &&
   grep -qF "$NAME_MARK" "$DEST/$LIB/$THREAD_RS" &&
   grep -A1 '^name = "libc"' "$DEST/$LIB/Cargo.lock" | grep -qF "$(sed -n 's/^version = "\(.*\)"/\1/p' "$LIBC/Cargo.toml" | head -1)"; then
  rustup toolchain link "$NAME" "$DEST" >&2
  echo "$NAME"
  exit 0
fi
rustup toolchain install "$BASE" --profile minimal --component rust-src >&2
SYSROOT=$(rustc +"$BASE" --print sysroot)
rm -rf "$DEST"; mkdir -p "$(dirname "$DEST")"
# hard links where the file system allows them (GNU cp), a plain copy otherwise; rustc finds its
# sysroot from its own path, so a symlinked tree would still point cargo at the original sources
cp -al "$SYSROOT" "$DEST" 2>/dev/null || { rm -rf "$DEST"; cp -a "$SYSROOT" "$DEST"; }
# the library sources are edited: a real copy, never a hard link into the base toolchain
rm -rf "$DEST/$LIB"; mkdir -p "$DEST/$LIB"; cp -R "$SYSROOT/$LIB/." "$DEST/$LIB/"
grep -q '^libc = { path' "$DEST/$LIB/Cargo.toml" ||
  printf '# RTEMS 6.2 + libbsd layouts (nexus2 vendor/libc)\nlibc = { path = "%s" }\n' "$LIBC" >> "$DEST/$LIB/Cargo.toml"
# std::thread names reach RTEMS: std treats every newlib target as having no thread names;
# RTEMS 6 has pthread_setname_np, so `top`, `cpuuse` and `nexus load` can show nexus2's
# thread names. Best effort: a toolchain whose source no longer matches only loses the names.
python3 - "$DEST/$LIB/$THREAD_RS" "$NAME_MARK" <<'PY' >&2 || echo "::warning::std thread-name patch not applied" >&2
import sys
path, mark = sys.argv[1], sys.argv[2]
src = open(path).read()
anchor = """    #[cfg(any(
        target_env = "newlib",
        target_os = "l4re","""
if mark in src:
    sys.exit(0)
if src.count(anchor) != 1:
    sys.exit("std thread-name anchor not found")
patched = """    // %s
    #[cfg(target_os = "rtems")]
    pub fn set_name(name: &CStr) {
        unsafe extern "C" {
            fn pthread_setname_np(thread: libc::pthread_t, name: *const libc::c_char) -> libc::c_int;
        }
        // SAFETY: NUL-terminated name; RTEMS copies and truncates it.
        unsafe { pthread_setname_np(libc::pthread_self(), name.as_ptr()) };
    }

    #[cfg(any(
        all(target_env = "newlib", not(target_os = "rtems")),
        target_os = "l4re",""" % mark
open(path, "w").write(src.replace(anchor, patched))
print("std: RTEMS thread names patched")
PY
rustup toolchain link "$NAME" "$DEST" >&2
# std's lock file pins libc to the registry; let cargo move that one entry to the path crate
# (every other entry stays as locked) so build-std resolves the patched workspace
(cd "$DEST/$LIB" && cargo +"$NAME" update -p libc --precise "$(sed -n 's/^version = "\(.*\)"/\1/p' "$LIBC/Cargo.toml" | head -1)" >&2 2>/dev/null ||
  cargo +"$NAME" metadata --format-version 1 >/dev/null)
grep -A2 '^name = "libc"' "$DEST/$LIB/Cargo.lock" >&2
echo "$NAME"
