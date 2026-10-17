#!/bin/sh
# Nano additions, run after board/pluto/post-build.sh (which installs the stock mdev.conf over the
# rootfs overlay's copy). $1 = target dir.
set -e
T=${1:?target dir}; HERE=$(cd "$(dirname "$0")" && pwd)
# mdev rules with the AIC8800 Wi-Fi mode switch (written into the overlay by build-aic8800.sh)
[ -f "$HERE/rootfs-overlay/etc/mdev.conf" ] && install -m 0644 "$HERE/rootfs-overlay/etc/mdev.conf" "$T/etc/mdev.conf"
# btop refuses to start without a UTF-8 locale; musl has C.UTF-8 built in, the shell just never sets it
if [ -f "$T/usr/bin/btop" ] && ! head -c 2 "$T/usr/bin/btop" | grep -q '#!'; then
  mkdir -p "$T/usr/libexec" && mv "$T/usr/bin/btop" "$T/usr/libexec/btop"
  printf '#!/bin/sh\nexport LANG=${LANG:-C.UTF-8} TERM=${TERM:-xterm-256color}\nexec /usr/libexec/btop "$@"\n' > "$T/usr/bin/btop"
  chmod 0755 "$T/usr/bin/btop"
fi
# the SoapyPlutoSDR module and SoapySDRUtil are the only users of these; keep headers/cmake out
rm -rf "$T/usr/share/cmake" "$T/usr/lib/cmake" "$T/usr/lib/pkgconfig" "$T/usr/include" 2>/dev/null || true
chmod 0755 "$T"/etc/init.d/S*
