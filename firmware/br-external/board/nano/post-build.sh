#!/bin/sh
# Nano additions, run after board/pluto/post-build.sh (which installs the stock mdev.conf over the
# rootfs overlay's copy). $1 = target dir.
set -e
T=${1:?target dir}; HERE=$(cd "$(dirname "$0")" && pwd)
# mdev rules (modalias module loading, usb_modeswitch, network hotplug): the overlay's copy, over the
# stock one board/pluto/post-build.sh installs after the overlay
install -m 0644 "$HERE/rootfs-overlay/etc/mdev.conf" "$T/etc/mdev.conf"
# mdev: events handled in kernel order (/dev/mdev.seq), and devices present before the hotplug
# helper was registered replayed once (coldplug: modules by alias, mode switches), like udevadm trigger
python3 - "$T/etc/init.d/S10mdev" <<'PY2'
import sys
p = sys.argv[1]; s = open(p).read()
a = "\techo /sbin/mdev >/proc/sys/kernel/hotplug\n\t/sbin/mdev -s\n"
assert s.count(a) == 1, "S10mdev: hotplug lines not found"
s = s.replace(a, "\techo > /dev/mdev.seq\n" + a +
    "\t# coldplug: replay the add events of devices that appeared before mdev was the hotplug helper\n"
    "\tfor u in /sys/bus/usb/devices/*/uevent; do [ -e \"$u\" ] && echo add > \"$u\"; done; true\n")
open(p, "w").write(s)
PY2
# NexusBS 2.0 login banner over the stock PlutoSDR one (S45msd fills in #BUILD#)
install -m 0644 "$HERE/motd" "$T/etc/motd"
# root's login shell is bash (SSH and serial console); init scripts keep busybox /bin/sh
[ -x "$T/bin/bash" ] && sed -i 's#^root:\([^:]*\):0:0:\([^:]*\):\([^:]*\):.*#root:\1:0:0:\2:\3:/bin/bash#' "$T/etc/passwd"
grep -qx /bin/bash "$T/etc/shells" 2>/dev/null || echo /bin/bash >> "$T/etc/shells"
# config.txt [SYSTEM] operating_mode = slave|master (station role, nexus-role): written by S40network,
# read back by update.sh like the stock keys and applied at once
python3 - "$T/etc/init.d/S40network" "$T/sbin/update.sh" "$T/etc/init.d/S45msd" <<'PY'
import sys
net, upd, msd = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(net).read()
def rep(old, new):
    global s
    assert s.count(old) == 1, "S40network: " + old
    s = s.replace(old, new)
# Wi-Fi regulatory country (config.txt [WLAN] country_wlan, default RO): cfg80211 + wireless-regdb
rep("\tWLAN_IPADDR=`fw_printenv -n ipaddr_wlan 2> /dev/null`\n",
    "\tWLAN_IPADDR=`fw_printenv -n ipaddr_wlan 2> /dev/null`\n"
    "\tWLAN_COUNTRY=`fw_printenv -n country_wlan 2> /dev/null`\n"
    "\techo \"$WLAN_COUNTRY\" | grep -qx '[A-Z][A-Z]' || WLAN_COUNTRY=RO\n")
rep("\techo \"ipaddr_wlan = $WLAN_IPADDR\"$'\\r' >> $CONF\n",
    "\techo \"ipaddr_wlan = $WLAN_IPADDR\"$'\\r' >> $CONF\n"
    "\techo \"# regulatory country (ISO 3166 alpha-2), default RO\"$'\\r' >> $CONF\n"
    "\techo \"country_wlan = $WLAN_COUNTRY\"$'\\r' >> $CONF\n")
# USB Ethernet and Wi-Fi adapters come and go: busybox ifupdown has no "allow-hotplug" keyword (it
# would be taken as an option of the previous stanza), so the stanzas stay "auto" (ifup -a at boot)
# and mdev's net rule runs ifup/ifdown when an adapter appears or goes. DHCP sends the host name (the
# router lists the Pluto under it) and udhcpc stays in the background until a lease arrives (-b takes
# priority over ifup's default -n)
rep('\t\techo -e "iface eth0 inet dhcp\\n" >> $IFAC\n',
    '\t\techo -e "iface eth0 inet dhcp\\n\\thostname $HOSTNAME\\n\\tudhcpc_opts -b\\n" >> $IFAC\n')
rep('\t\t\techo -e "iface wlan0 inet dhcp" >> $IFAC\n',
    '\t\t\techo -e "iface wlan0 inet dhcp\\n\\thostname $HOSTNAME\\n\\tudhcpc_opts -b" >> $IFAC\n')
# country before any interface comes up; resolv.conf (a link into /tmp) exists for avahi/udhcpc
# network.json (nexus-net, /mnt/data/network) wins over the config.txt/U-Boot Wi-Fi settings when present
rep("\t/sbin/ifup -a 2>&1 | logger\n",
    "\t[ -x /usr/sbin/nexus-net ] && /usr/sbin/nexus-net render-boot\n\t/sbin/ifup -a 2>&1 | logger\n")
rep("\t/bin/hostname -F /etc/hostname\n",
    "\t/bin/hostname -F /etc/hostname\n"
    "\tiw reg set \"$WLAN_COUNTRY\" 2>&1 | logger -t network\n"
    "\tR=`readlink -f /etc/resolv.conf`; [ -e \"$R\" ] || : > \"$R\"\n")
a = "\tUSB_ETH_MODE=`fw_printenv -n usb_ethernet_mode 2> /dev/null || echo rndis`\n"
assert s.count(a) == 1, "S40network: usb_ethernet_mode read not found"
s = s.replace(a, a + "\tOP_MODE=`fw_printenv -n operating_mode 2> /dev/null || echo slave`\n")
b = "\techo \"usb_ethernet_mode = $USB_ETH_MODE\"$'\\r' >> $CONF\n"
assert s.count(b) == 1, "S40network: usb_ethernet_mode line not found"
s = s.replace(b, b + "\techo \"# NexusBS station role [slave|master]: slave = SDR for NexusBS on another computer,\"$'\\r' >> $CONF\n"
                     "\techo \"# master = NexusBS runs on this Pluto\"$'\\r' >> $CONF\n"
                     "\techo \"operating_mode = $OP_MODE\"$'\\r' >> $CONF\n")
# config.txt [SYSTEM] usb_mode (S96usbrole): peripheral (default) | auto | host
rep("\tOP_MODE=`fw_printenv -n operating_mode 2> /dev/null || echo slave`\n",
    "\tOP_MODE=`fw_printenv -n operating_mode 2> /dev/null || echo slave`\n"
    "\tUSB_MODE=`fw_printenv -n usb_mode 2> /dev/null || echo peripheral`\n")
# config.txt [SYSTEM] cpu_clock / cpu_clock_cooling_confirmed (nexus-clock, S22clock)
rep("\tUSB_MODE=`fw_printenv -n usb_mode 2> /dev/null || echo peripheral`\n",
    "\tUSB_MODE=`fw_printenv -n usb_mode 2> /dev/null || echo peripheral`\n"
    "\tCPU_CLOCK=`fw_printenv -n cpu_clock 2> /dev/null`; case \"$CPU_CLOCK\" in standard|495_396|950_600|1100_750) ;; *) CPU_CLOCK=standard ;; esac\n"
    "\tCPU_COOL=`fw_printenv -n cpu_clock_cooling_confirmed 2> /dev/null`; [ \"$CPU_COOL\" = yes ] || CPU_COOL=no\n")
rep("\techo \"operating_mode = $OP_MODE\"$'\\r' >> $CONF\n",
    "\techo \"operating_mode = $OP_MODE\"$'\\r' >> $CONF\n"
    "\techo \"# USB-C role [peripheral|auto|host]: peripheral = a computer is the host (default), host = hub with a\"$'\\r' >> $CONF\n"
    "\techo \"# Wi-Fi/Ethernet adapter, auto = host when no computer configures the port (applied at the next boot)\"$'\\r' >> $CONF\n"
    "\techo \"usb_mode = $USB_MODE\"$'\\r' >> $CONF\n"
    "\techo \"# CPU/DDR clock [standard|495_396|950_600|1100_750], applied at the next boot by rewriting the 64 KiB\"$'\\r' >> $CONF\n"
    "\techo \"# FSBL block (< 1 s; a power cut during it needs DFU/JTAG recovery). standard = Tezuka default.\"$'\\r' >> $CONF\n"
    "\techo \"# 1100_750 = CPU 1100 MHz / DDR 750 MT/s: tested on the reference board (HamGeek Pluto Nano Rev.B)\"$'\\r' >> $CONF\n"
    "\techo \"# with heatsinks and a fan installed only, above the -1 speed grade spec; active cooling required,\"$'\\r' >> $CONF\n"
    "\techo \"# set cpu_clock_cooling_confirmed = yes or it is refused. 495_396 / 950_600: Tezuka files, not tested\"$'\\r' >> $CONF\n"
    "\techo \"# on the reference board.\"$'\\r' >> $CONF\n"
    "\techo \"cpu_clock = $CPU_CLOCK\"$'\\r' >> $CONF\n"
    "\techo \"cpu_clock_cooling_confirmed = $CPU_COOL\"$'\\r' >> $CONF\n"
    "\techo \"# Bluetooth controller at boot [on|off] (S39bluetooth; off also starts the NexusBS Player switch off)\"$'\\r' >> $CONF\n"
    "\techo \"bluetooth = $BT_STATE\"$'\\r' >> $CONF\n")
# config.txt [SYSTEM] bluetooth (S39bluetooth, nexus-config): on (default) | off
rep("\tCPU_COOL=`fw_printenv -n cpu_clock_cooling_confirmed 2> /dev/null`; [ \"$CPU_COOL\" = yes ] || CPU_COOL=no\n",
    "\tCPU_COOL=`fw_printenv -n cpu_clock_cooling_confirmed 2> /dev/null`; [ \"$CPU_COOL\" = yes ] || CPU_COOL=no\n"
    "\tBT_STATE=`fw_printenv -n bluetooth 2> /dev/null`; [ \"$BT_STATE\" = off ] || BT_STATE=on\n")
# network.json (nexus-config) replaces the Wi-Fi/Ethernet keys of config.txt: say so in the file itself
rep("\techo \"[WLAN]\"$'\\r' >> $CONF\n",
    "\techo \"[WLAN]\"$'\\r' >> $CONF\n"
    "\t[ -f /mnt/data/network/network.json ] && echo \"# network.json is in use (nexus-config): ssid_wlan, pwd_wlan, ipaddr_wlan and the [USB_ETHERNET] address are ignored\"$'\\r' >> $CONF\n")
open(net, "w").write(s)
u = open(upd).read()
w = "\t\techo \"pwd_wlan $pwd_wlan\" >> /opt/fw_set.tmp\n"
assert u.count(w) == 1, "update.sh: pwd_wlan line not found"
u = u.replace(w, w + "\t\techo \"country_wlan $country_wlan\" >> /opt/fw_set.tmp\n")
c = "\t\techo \"netmask_eth $netmask_eth\" >> /opt/fw_set.tmp\n"
assert u.count(c) == 1, "update.sh: netmask_eth line not found"
u = u.replace(c, c + "\t\tcase \"$operating_mode\" in master|slave) ;; *) operating_mode=`fw_printenv -n operating_mode 2> /dev/null || echo slave` ;; esac\n"
                     "\t\techo \"operating_mode $operating_mode\" >> /opt/fw_set.tmp\n"
                     "\t\tcase \"$usb_mode\" in peripheral|auto|host) ;; *) usb_mode=`fw_printenv -n usb_mode 2> /dev/null || echo peripheral` ;; esac\n"
                     "\t\techo \"usb_mode $usb_mode\" >> /opt/fw_set.tmp\n"
                     "\t\told_bt=`fw_printenv -n bluetooth 2> /dev/null`; [ \"$old_bt\" = off ] || old_bt=on\n"
                     "\t\tcase \"$bluetooth\" in on|off) ;; *) bluetooth=$old_bt ;; esac\n"
                     "\t\techo \"bluetooth $bluetooth\" >> /opt/fw_set.tmp\n"
                     "\t\told_clock=`fw_printenv -n cpu_clock 2> /dev/null || echo standard`\n"
                     "\t\told_cool=`fw_printenv -n cpu_clock_cooling_confirmed 2> /dev/null || echo no`\n"
                     "\t\tcase \"$cpu_clock\" in standard|495_396|950_600|1100_750) ;; *) cpu_clock=$old_clock ;; esac\n"
                     "\t\tcase \"$cpu_clock_cooling_confirmed\" in yes|no) ;; *) cpu_clock_cooling_confirmed=$old_cool ;; esac\n"
                     "\t\tif [ \"$cpu_clock\" = 1100_750 ] && [ \"$cpu_clock_cooling_confirmed\" != yes ]; then logger -t nexus-clock \"cpu_clock 1100_750 refused: cpu_clock_cooling_confirmed is not yes\"; cpu_clock=$old_clock; fi\n"
                     "\t\techo \"cpu_clock $cpu_clock\" >> /opt/fw_set.tmp\n"
                     "\t\techo \"cpu_clock_cooling_confirmed $cpu_clock_cooling_confirmed\" >> /opt/fw_set.tmp\n"
                     # the variant S22clock installs (nexus-clock configured): 1100_750 needs the cooling confirmation
                     "\t\tclock_eff() { case \"$1\" in standard|495_396|950_600) echo $1 ;; 1100_750) [ \"$2\" = yes ] && echo $1 || echo standard ;; *) echo standard ;; esac; }\n"
                     "\t\t[ \"`clock_eff $cpu_clock $cpu_clock_cooling_confirmed`\" != \"`clock_eff $old_clock $old_cool`\" ] && do_reset=1\n")
d = "\t\ttouch /mnt/msd/SUCCESS_ENV_UPDATE\n"
assert u.count(d) == 1, "update.sh: SUCCESS_ENV_UPDATE not found"
u = u.replace(d, d + "\t\t[ -x /usr/sbin/nexus-role ] && /usr/sbin/nexus-role \"$operating_mode\" > /dev/null 2>&1\n"
                     "\t\t[ \"$bluetooth\" != \"$old_bt\" ] && [ -x /usr/sbin/nexus-config ] && /usr/sbin/nexus-config nonint do_bluetooth \"$bluetooth\" > /dev/null 2>&1\n")
# boot*.frm / apps*.frm variants (same U-Boot/env or payload, different FSBL clock): exactly one of each
# is accepted; several on the drive refuse the whole update (nothing is written) and are removed
f = "\tif [[ -s ${FIRMWARE} ]]\n\tthen\n\t\thandle_frimware_frm \"${FIRMWARE}\" \"${FRM_MAGIC}\"\n\tfi\n"
assert u.count(f) == 1, "update.sh: pluto.frm block not found"
u = u.replace(f,
    "\t# exactly one boot*.frm and one apps*.frm: several = the whole update is refused\n"
    "\tbootimage=/mnt/msd/boot.frm; appsimage=; nb=0; na=0\n"
    "\tfor f in /mnt/msd/boot*.frm; do case \"$f\" in */._*) ;; *) [ -s \"$f\" ] && nb=$((nb+1)) && bootimage=$f ;; esac; done\n"
    "\tfor f in /mnt/msd/apps*.frm; do case \"$f\" in */._*) ;; *) [ -s \"$f\" ] && na=$((na+1)) && appsimage=$f ;; esac; done\n"
    "\tif [ $nb -gt 1 ] || [ $na -gt 1 ]\n\tthen\n"
    "\t\tls /mnt/msd/*.frm > /mnt/msd/FAILED_SEVERAL_FRM_FILES 2>&1\n"
    "\t\trm -f /mnt/msd/*.frm; bootimage=/mnt/msd/boot.frm; appsimage=\n\tfi\n" + f)
# apps.frm (application slot, apps-frm-install) and nexus-setup.txt (copied to /mnt/data/nexus2/setup.txt
# for the application's provision.sh, then removed from the drive), after pluto.frm and boot.frm
e = "\tif [[ -s ${bootimage} ]]\n\tthen\n\t\thandle_boot_frm \"${bootimage}\"\n\tfi\n"
assert u.count(e) == 1, "update.sh: boot.frm block not found"
u = u.replace(e, e +
    "\n\tif [[ -n \"${appsimage}\" ]] && [[ -s \"${appsimage}\" ]]\n\tthen\n\t\tflash_indication_on\n"
    "\t\t/usr/sbin/apps-frm-install \"${appsimage}\" && do_reset=1\n"
    "\t\tflash_indication_off\n\t\trm -f \"${appsimage}\" /mnt/msd/._apps*.frm\n\tfi\n"
    "\n\tif [[ -s /mnt/msd/nexus-setup.txt ]]\n\tthen\n"
    "\t\tif grep -q ' /mnt/data ubifs ' /proc/mounts\n\t\tthen\n"
    "\t\t\tmkdir -p -m 0700 /mnt/data/nexus2 && cp /mnt/msd/nexus-setup.txt /mnt/data/nexus2/setup.txt && chmod 0600 /mnt/data/nexus2/setup.txt && sync && touch /mnt/msd/SETUP_SUCCESS && do_reset=1 || touch /mnt/msd/SETUP_FAILED\n"
    "\t\telse\n\t\t\techo 'no persistent data partition' > /mnt/msd/SETUP_FAILED\n\t\tfi\n"
    "\t\trm -f /mnt/msd/nexus-setup.txt /mnt/msd/._nexus-setup.txt\n\tfi\n")
# nexus-config writes the U-Boot environment directly; the drive keeps the config.txt of the last boot
# until the next one. A key the operator did not edit on the drive (same value as the copy S45msd put
# there, /opt/config.drive) keeps the current environment value, so an unrelated config.txt edit never
# reverts a nexus-config setting.
g = "\tini_parser $FILE \"USB_ETHERNET\"\n"
assert u.count(g) == 1, "update.sh: USB_ETHERNET parser line not found"
u = u.replace(g, g +
    "\tif [ -f /opt/config.drive ]; then\n"
    "\t\tfor k in hostname xo_correction country_wlan operating_mode usb_mode cpu_clock cpu_clock_cooling_confirmed bluetooth; do\n"
    "\t\t\td=`sed -n \"s/^$k[[:space:]]*=[[:space:]]*//p\" /opt/config.drive | tr -d '\\r' | sed 's/[[:space:]]*$//' | head -1`\n"
    "\t\t\teval \"n=\\${$k}\"\n"
    "\t\t\t[ \"$n\" = \"$d\" ] && eval \"$k=\\`fw_printenv -n $k 2> /dev/null\\`\"\n"
    "\t\tdone\n\tfi\n")
h = "\tmd5sum $FILE > /opt/config.md5\n"
assert u.count(h) == 1, "update.sh: config.md5 line not found"
u = u.replace(h, h + "\tcp $FILE /opt/config.drive\n")
open(upd, "w").write(u)
m = open(msd).read()
i = "\t\tcp $CONF /mnt/msd\n"
assert m.count(i) == 1, "S45msd: config.txt copy not found"
open(msd, "w").write(m.replace(i, i + "\t\tcp $CONF /opt/config.drive\n"))
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
# master role: no iiod either (NexusBS owns the radio; iiod next to it would share the AD9361), so the
# IIO FunctionFS function leaves the composite with it (a FunctionFS function without its daemon blocks the bind)
rep("\tmkdir -p $GADGET/functions/ffs.iio_ffs\n", "\t[ $SDRG = 1 ] && mkdir -p $GADGET/functions/ffs.iio_ffs\n")
rep("\tln -s $GADGET/functions/ffs.iio_ffs $GADGET/configs/c.1/ffs.iio_ffs\n",
    "\t[ $SDRG = 1 ] && ln -s $GADGET/functions/ffs.iio_ffs $GADGET/configs/c.1/ffs.iio_ffs\n")
rep("\tmkdir -p /dev/iio_ffs\n\tmount iio_ffs -t functionfs /dev/iio_ffs 2> /dev/null\n",
    "\t[ $SDRG = 1 ] && mkdir -p /dev/iio_ffs && mount iio_ffs -t functionfs /dev/iio_ffs 2> /dev/null\n")
rep("\tstart-stop-daemon -S -b -q -m -p /var/run/iiod.pid -x  /usr/sbin/iiod  -- $IIOD_OPTS\n",
    "\t[ $SDRG = 1 ] && start-stop-daemon -S -b -q -m -p /var/run/iiod.pid -x  /usr/sbin/iiod  -- $IIOD_OPTS\n")
rep("\trm $GADGET/configs/c.1/ffs.iio_ffs\n", "\trm -f $GADGET/configs/c.1/ffs.iio_ffs\n")
rep("\trmdir $GADGET/functions/ffs.iio_ffs\n", "\trmdir $GADGET/functions/ffs.iio_ffs 2> /dev/null\n")
open(p, "w").write(s)
PY
# mount point of the persistent data partition (S21data)
mkdir -p "$T/mnt/data"
# SSH host keys persist on the data partition: generated once (dropbear -R) and kept across reboots
rm -rf "$T/etc/dropbear" && ln -s /mnt/data/dropbear "$T/etc/dropbear"
# Bluetooth pairings (bluetoothd) persist on the data partition, as with the former BT add-on
rm -rf "$T/var/lib/bluetooth" && ln -s /mnt/data/bt/lib "$T/var/lib/bluetooth"
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
# alsa-lib only comes with the bluez-alsa package (its ALSA PCM/CTL plugins); bluealsa, bluealsa-cli
# and the applications use D-Bus and PCM pipes, not ALSA. Drop alsa-lib, its tools and the plugins
# unless some other program links against it.
if ! grep -rlsq --exclude='libasound.so*' --exclude='libatopology.so*' --exclude=aserver --exclude-dir=alsa-lib \
     'libasound.so' "$T/usr/bin" "$T/usr/sbin" "$T/usr/lib" "$T/usr/libexec" "$T/lib" "$T/bin" "$T/sbin"; then
  rm -rf "$T"/usr/lib/libasound.so* "$T"/usr/lib/libatopology.so* "$T/usr/bin/aserver" "$T/usr/lib/alsa-lib" \
         "$T/usr/share/alsa" "$T/etc/alsa"
fi
# usb_modeswitch is called directly by mdev with its configuration file; the udev dispatcher and its
# Jim Tcl interpreter are not used
rm -f "$T/usr/sbin/usb_modeswitch_dispatcher" "$T/lib/udev/usb_modeswitch"; rmdir "$T/lib/udev" 2>/dev/null || true
# development and test tools pulled in by libraries (Jim Tcl shell, PCRE/libxml2/GLib/D-Bus test and
# schema tools); not used on the device
rm -f "$T"/usr/bin/jimsh "$T"/usr/bin/tclsh "$T"/usr/bin/pcretest "$T"/usr/bin/pcregrep "$T"/usr/bin/xmllint \
      "$T"/usr/bin/xmlcatalog "$T"/usr/bin/gio "$T"/usr/bin/gio-querymodules "$T"/usr/bin/gapplication \
      "$T"/usr/bin/gresource "$T"/usr/bin/gsettings "$T"/usr/bin/dbus-test-tool "$T"/usr/bin/a2dpconf \
      "$T"/usr/lib/libstdc++.so.*-gdb.py
rm -rf "$T/usr/share/gdb"
# libraries nothing links against any more
for lib in libgfortran libgomp libjim libform libmenu libpanel libnl-xfrm-3 libnl-idiag-3 libnl-nf-3 \
           libavahi-gobject libgpiodcxx libpcrecpp libpcreposix; do
  if ! grep -rlsq --exclude="$lib.so*" "$lib.so" "$T/usr" "$T/lib" "$T/bin" "$T/sbin"; then rm -f "$T"/usr/lib/$lib.so* "$T"/lib/$lib.so*; fi
done
chmod 0755 "$T"/etc/init.d/S*
