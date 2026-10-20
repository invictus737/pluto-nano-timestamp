#!/bin/sh
# Nano additions, run after board/pluto/post-build.sh (which installs the stock mdev.conf over the
# rootfs overlay's copy). $1 = target dir.
set -e
T=${1:?target dir}; HERE=$(cd "$(dirname "$0")" && pwd)
# mdev rules with the AIC8800 Wi-Fi mode switch (written into the overlay by build-aic8800.sh)
[ -f "$HERE/rootfs-overlay/etc/mdev.conf" ] && install -m 0644 "$HERE/rootfs-overlay/etc/mdev.conf" "$T/etc/mdev.conf"
# network hotplug for every USB adapter (DHCP retry while Wi-Fi associates), over the stock helper
install -m 0755 "$HERE/ifupdown.sh" "$T/lib/mdev/ifupdown.sh"
# NexusBS 2.0 login banner over the stock PlutoSDR one (S45msd fills in #BUILD#)
install -m 0644 "$HERE/motd" "$T/etc/motd"
# root's login shell is bash (SSH and serial console); init scripts keep busybox /bin/sh
[ -x "$T/bin/bash" ] && sed -i 's#^root:\([^:]*\):0:0:\([^:]*\):\([^:]*\):.*#root:\1:0:0:\2:\3:/bin/bash#' "$T/etc/passwd"
grep -qx /bin/bash "$T/etc/shells" 2>/dev/null || echo /bin/bash >> "$T/etc/shells"
# mount point of the persistent data partition (S21data)
mkdir -p "$T/mnt/data"
# btop refuses to start without a UTF-8 locale; musl has C.UTF-8 built in, the shell just never sets it
if [ -f "$T/usr/bin/btop" ] && ! head -c 2 "$T/usr/bin/btop" | grep -q '#!'; then
  mkdir -p "$T/usr/libexec" && mv "$T/usr/bin/btop" "$T/usr/libexec/btop"
  printf '#!/bin/sh\nexport LANG=${LANG:-C.UTF-8} TERM=${TERM:-xterm-256color}\nexec /usr/libexec/btop "$@"\n' > "$T/usr/bin/btop"
  chmod 0755 "$T/usr/bin/btop"
fi
# the SoapyPlutoSDR module and SoapySDRUtil are the only users of these; keep headers/cmake out
rm -rf "$T/usr/share/cmake" "$T/usr/lib/cmake" "$T/usr/lib/pkgconfig" "$T/usr/include" 2>/dev/null || true
# fw_setenv on an environment with a bad CRC writes the tools' generic default (bootcmd=bootp, three
# variables) with a valid CRC: U-Boot then has no qspiboot and no DFU button, and the board no longer
# boots. The stock frm update calls fw_setenv fit_size. Refuse instead; U-Boot itself boots fine from
# its built-in environment until the partition is restored (DFU uboot-env.dfu).
if [ -L "$T/usr/sbin/fw_setenv" ]; then
  mkdir -p "$T/usr/libexec" && rm "$T/usr/sbin/fw_setenv" && ln -sf ../sbin/fw_printenv "$T/usr/libexec/fw_setenv"
  cat > "$T/usr/sbin/fw_setenv" <<'SH'
#!/bin/sh
if fw_printenv 2>&1 >/dev/null | grep -q 'Bad CRC'; then
  echo "fw_setenv: the U-Boot environment partition is invalid (Bad CRC); not replacing it with a generic one" >&2
  logger -t fw_setenv "refused: environment has a bad CRC ($*)"; exit 1
fi
exec /usr/libexec/fw_setenv "$@"
SH
  chmod 0755 "$T/usr/sbin/fw_setenv"
fi
# macOS metadata copied in with overlay files
find "$T" -name '._*' -delete
# libraries nothing links against (pulled in by the toolchain's OpenMP/Fortran runtime and fftw's
# threaded variants; ad936x_ref_cal only needs libfftw3), and the fftw wisdom tools
rm -f "$T"/usr/bin/fftw*-wisdom* "$T"/usr/lib/libfftw3*_omp.so* "$T"/usr/lib/libfftw3*_threads.so*
for lib in libgfortran libgomp; do
  if ! grep -rlsq --exclude="$lib.so*" "$lib.so" "$T/usr" "$T/lib" "$T/bin" "$T/sbin"; then rm -f "$T"/usr/lib/$lib.so* "$T"/lib/$lib.so*; fi
done
chmod 0755 "$T"/etc/init.d/S*
