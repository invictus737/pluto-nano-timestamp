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
# config.txt [SYSTEM] operating_mode = slave|master (station role, nexus-role): written by S40network,
# read back by update.sh like the stock keys and applied at once
python3 - "$T/etc/init.d/S40network" "$T/sbin/update.sh" <<'PY'
import sys
net, upd = sys.argv[1], sys.argv[2]
s = open(net).read()
a = "\tUSB_ETH_MODE=`fw_printenv -n usb_ethernet_mode 2> /dev/null || echo rndis`\n"
assert s.count(a) == 1, "S40network: usb_ethernet_mode read not found"
s = s.replace(a, a + "\tOP_MODE=`fw_printenv -n operating_mode 2> /dev/null || echo slave`\n")
b = "\techo \"usb_ethernet_mode = $USB_ETH_MODE\"$'\\r' >> $CONF\n"
assert s.count(b) == 1, "S40network: usb_ethernet_mode line not found"
s = s.replace(b, b + "\techo \"# NexusBS station role [slave|master]: slave = SDR for NexusBS on another computer,\"$'\\r' >> $CONF\n"
                     "\techo \"# master = NexusBS runs on this Pluto\"$'\\r' >> $CONF\n"
                     "\techo \"operating_mode = $OP_MODE\"$'\\r' >> $CONF\n")
open(net, "w").write(s)
u = open(upd).read()
c = "\t\techo \"netmask_eth $netmask_eth\" >> /opt/fw_set.tmp\n"
assert u.count(c) == 1, "update.sh: netmask_eth line not found"
u = u.replace(c, c + "\t\tcase \"$operating_mode\" in master) ;; *) operating_mode=slave ;; esac\n"
                     "\t\techo \"operating_mode $operating_mode\" >> /opt/fw_set.tmp\n")
d = "\t\ttouch /mnt/msd/SUCCESS_ENV_UPDATE\n"
assert u.count(d) == 1, "update.sh: SUCCESS_ENV_UPDATE not found"
u = u.replace(d, d + "\t\t[ -x /usr/sbin/nexus-role ] && /usr/sbin/nexus-role \"$operating_mode\" > /dev/null 2>&1\n")
open(upd, "w").write(u)
PY
# master role: no SDR gadgets (NexusBS on the Pluto uses the radio directly). The USB SDR function must
# not even be in the composite then: a FunctionFS function without its daemon blocks the UDC bind.
python3 - "$T/etc/init.d/S23udc" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
def rep(old, new):
    global s
    assert s.count(old) == 1, "S23udc: " + old
    s = s.replace(old, new)
rep("SDR_USB_GADGET_DEBUG=`fw_printenv -n sdr_usb_gadget_debug 2> /dev/null || echo 0`\n",
    "SDR_USB_GADGET_DEBUG=`fw_printenv -n sdr_usb_gadget_debug 2> /dev/null || echo 0`\n"
    "# station role (config.txt operating_mode): the SDR USB gadget only as a slave\n"
    "SDRG=1; [ \"`fw_printenv -n operating_mode 2> /dev/null`\" = master ] && SDRG=0\n")
rep("\tmkdir -p $GADGET/functions/ffs.sdr_gadget_ffs\n", "\t[ $SDRG = 1 ] && mkdir -p $GADGET/functions/ffs.sdr_gadget_ffs\n")
rep("\tln -s $GADGET/functions/ffs.sdr_gadget_ffs $GADGET/configs/c.1/ffs.sdr_gadget_ffs\n",
    "\t[ $SDRG = 1 ] && ln -s $GADGET/functions/ffs.sdr_gadget_ffs $GADGET/configs/c.1/ffs.sdr_gadget_ffs\n")
rep("\tmkdir -p /dev/sdr_gadget_ffs\n\tmount sdr_gadget_ffs -t functionfs /dev/sdr_gadget_ffs 2> /dev/null\n",
    "\t[ $SDRG = 1 ] && mkdir -p /dev/sdr_gadget_ffs && mount sdr_gadget_ffs -t functionfs /dev/sdr_gadget_ffs 2> /dev/null\n")
rep("\tif [ \"$SDR_USB_GADGET_DEBUG\" == \"1\" ]\n", "\tif [ $SDRG = 0 ]\n\tthen\n\t\t:\n\telif [ \"$SDR_USB_GADGET_DEBUG\" == \"1\" ]\n")
rep("\trm $GADGET/configs/c.1/ffs.sdr_gadget_ffs\n", "\trm -f $GADGET/configs/c.1/ffs.sdr_gadget_ffs\n")
rep("\trmdir $GADGET/functions/ffs.sdr_gadget_ffs\n", "\trmdir $GADGET/functions/ffs.sdr_gadget_ffs 2> /dev/null\n")
open(p, "w").write(s)
PY
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
