#!/bin/sh
# mdev block device rule (sd*): the stock automounter, except for the AIC8800 "driver CD" (a69c:572x,
# 1111:1111). That disk only exists while aic-modeswitch.sh ejects it to switch the adapter to Wi-Fi
# mode; it is never mounted under /media (nor are its runme* files run).
P=${NEXUS_ROOT:-}   # tests only
if [ "${ACTION:-add}" != remove ] && [ -n "${MDEV:-}" ]; then
	d=$(readlink -f "$P/sys/class/block/$MDEV" 2>/dev/null)
	while [ -n "$d" ] && [ "$d" != "$P/sys" ] && [ "$d" != / ] && [ "$d" != . ]; do
		if [ -f "$d/idVendor" ] && [ -f "$d/idProduct" ]; then
			case "$(cat "$d/idVendor"):$(cat "$d/idProduct")" in
			a69c:572[1-7acf]|1111:1111) logger -t aic-modeswitch "$MDEV: AIC driver CD, not mounted"; exit 0 ;;
			esac
			break
		fi
		d=${d%/*}
	done
fi
exec "$P/lib/mdev/automounter.sh" "$@"
