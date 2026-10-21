#!/usr/bin/env bash
# Assemble pluto.frm / pluto.dfu from a prebuilt Nano system_top.xsa (no Vivado needed here).
# usage: build-fw.sh <plutosdr-fw checkout> <system_top.xsa>
set -euo pipefail
FW=${1:?plutosdr-fw checkout}; XSA=${2:?system_top.xsa}; HERE=$(cd "$(dirname "$0")" && pwd)
"$HERE/apply-nano.sh" "$FW"
# generated rootfs overlay (kernel modules, AIC8800 firmware; btop from build-btop.sh), the second
# entry of BR2_ROOTFS_OVERLAY
OVLG=$HERE/work-overlay; mkdir -p "$OVLG"
# AIC8800 Wi-Fi driver into the kernel tree (must precede the kernel configuration)
"$HERE/aic8800-intree.sh" "$FW/linux" "$OVLG"
mkdir -p "$FW/build" && cp "$XSA" "$FW/build/system_top.xsa"
# kernel: merge the Nano fragment into the Pluto defconfig
cat "$HERE/kernel-nano.config" >> "$FW/linux/arch/arm/configs/zynq_pluto_defconfig"
# buildroot: external tree (aic8800d80) + lean fragment merged into the Pluto defconfig
cat "$HERE/br-external/configs/pluto_nano_fragment.config" >> "$FW/buildroot/configs/zynq_pluto_defconfig"
export BR2_EXTERNAL="$HERE/br-external"
# cached across runs by .github/actions/fw-caches
cat >> "$FW/buildroot/configs/zynq_pluto_defconfig" <<BR
BR2_DL_DIR="$HOME/br-dl"
BR2_CCACHE=y
BR2_CCACHE_DIR="$HOME/.buildroot-ccache"
BR2_CCACHE_USE_BASEDIR=y
BR2_PER_PACKAGE_DIRECTORIES=y
BR
# firmware version (/opt/VERSIONS device-fw, login banner, msd page): the patched tree would describe
# itself as v0.39-dirty
sed -i 's/^VERSION=$(shell git describe .*/VERSION=v0.39-nexusBS/' "$FW/Makefile"
grep -q '^VERSION=v0.39-nexusBS$' "$FW/Makefile" || { echo "plutosdr-fw Makefile: VERSION line not found"; exit 1; }
# bluez-alsa: Buildroot builds bluealsa-aplay (an ALSA player, not used: applications write PCM through
# bluealsa-cli/D-Bus) but not bluealsa-cli; turn that around
python3 - "$FW/buildroot/package/bluez-alsa/bluez-alsa.mk" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
assert s.count("\t--enable-aplay \\\n") == 1, "bluez-alsa.mk: --enable-aplay not found"
open(p, "w").write(s.replace("\t--enable-aplay \\\n", "\t--disable-aplay \\\n\t--enable-cli \\\n"))
PY
grep -n -e '--enable-cli' "$FW/buildroot/package/bluez-alsa/bluez-alsa.mk"
# top-level parallel Buildroot (independent packages at the same time; needs per-package directories)
python3 - "$FW/Makefile" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read(); old = "\tmake -C buildroot BUSYBOX_CONFIG_FILE="
assert s.count(old) == 1, "plutosdr-fw Makefile: buildroot 'all' call not found"
open(p, "w").write(s.replace(old, "\tmake -C buildroot -j$$(nproc) BUSYBOX_CONFIG_FILE="))
PY
# Kernel and U-Boot keep ADI's compiler (Linaro GCC 7.3 arm-linux-gnueabihf). plutosdr-fw used to take
# it from Buildroot's external toolchain; the rootfs is musl now, so provide it separately on PATH.
LINARO=gcc-linaro-7.3.1-2018.05-x86_64_arm-linux-gnueabihf
if [ ! -x "$HERE/work-linaro/$LINARO/bin/arm-linux-gnueabihf-gcc" ]; then
  mkdir -p "$HERE/work-linaro"
  curl -sL "https://sources.buildroot.net/toolchain-external-linaro-arm/$LINARO.tar.xz" | tar xJ -C "$HERE/work-linaro"
fi
export PATH="$HERE/work-linaro/$LINARO/bin:$PATH"
arm-linux-gnueabihf-gcc --version | head -1
# zstd ramdisk: the plutosdr-fw Makefile and FIT source name rootfs.cpio.gz and mark it gzip
sed -i 's#rootfs\.cpio\.gz#rootfs.cpio.zst#g' "$FW/Makefile"
python3 - "$FW/scripts/pluto.its" <<'PY'
import re, sys
p = sys.argv[1]; s = open(p).read()
s, n = re.subn(r'(ramdisk@1 \{[^}]*?)rootfs\.cpio\.gz("\);[^}]*?compression = )"gzip"', r'\1rootfs.cpio.zst\2"none"', s, flags=re.S)
assert n == 1, "ramdisk@1 node not found in " + p
open(p, 'w').write(s)
PY
grep -n -A6 'ramdisk@1 {' "$FW/scripts/pluto.its" | head -8
# no generated legal-info in the image; the mass-storage layout (genimage-msd.cfg) still lists a
# LICENSE.html, so a short one points to the sources instead
export SKIP_LEGAL=1
cat > "$FW/buildroot/board/pluto/msd/LICENSE.html" <<'HTML'
<!DOCTYPE html><html><head><meta charset="utf-8"><title>Licenses</title></head><body>
<p>This firmware contains free software (Linux, U-Boot, Buildroot packages, libiio, SoapySDR and others)
under their own licenses, mostly GPL-2.0 and LGPL. Sources: https://github.com/analogdevicesinc/plutosdr-fw,
https://github.com/pgreenland/plutosdr-fw and the package URLs in Buildroot 2023.02.</p></body></html>
HTML
cd "$FW" && export TARGET=pluto VIVADO_SETTINGS=/nonexistent
make build/zImage 2>&1 | tail -5
# kernel modules (AIC8800 and the few others) with modules.dep/modules.alias into the overlay
"$HERE/build-modules.sh" "$FW/linux" "$OVLG"
if ! make build/pluto.frm > "$HERE/../fw-build.log" 2>&1; then
  # the real compiler/linker errors, not just make's tail
  grep -n -E "error:|undefined reference|No such file|cannot find|CMake Error" "$HERE/../fw-build.log" | head -40
  tail -30 "$HERE/../fw-build.log"; exit 1
fi
tail -5 "$HERE/../fw-build.log"
ls -la build/pluto.frm; sha256sum build/pluto.frm
# qspi-linux is 14 MiB (0x200000..0x1000000, the lower bank of the W25Q256): the FIT must fit, else
# U-Boot's read crosses into the upper bank (boot copy + data partition)
[ "$(stat -c %s build/pluto.frm)" -le $((14 * 1024 * 1024)) ] || { echo "pluto.frm is larger than the 14 MiB qspi-linux partition"; exit 1; }
# size report: FIT images, ramdisk content
{ echo "== FIT"; dumpimage -l build/pluto.itb 2>/dev/null || dumpimage -l build/pluto.frm
  ls -la build/zImage build/rootfs.cpio.zst build/pluto.frm build/pluto.dfu 2>/dev/null
  echo "== rootfs (uncompressed, KiB)"; du -sk buildroot/output/target
  du -k -a buildroot/output/target | sort -rn | awk '$1 >= 100' | head -60; } | tee "$HERE/../fw-size-report.txt"
# and a safety margin under U-Boot's 14 MiB fit_size window (a 19.66 MB image bricked a board)
[ "$(stat -c %s build/pluto.frm)" -le $((27 * 512 * 1024)) ] || { echo "pluto.frm is larger than the 13.5 MiB target"; exit 1; }
