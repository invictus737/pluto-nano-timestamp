#!/usr/bin/env bash
# Assemble apps.frm (application slot, see README "Application slot") from prepared inputs. Linux/CI.
#   apps.frm = 4096-byte header | payload (ustar tar, xz --check=crc32) | [boot.bin] | md5 trailer (33 B)
# usage: build-apps-frm.sh <inputs dir> <output dir>
# Outputs (same payload, only the carried boot.bin differs):
#   apps.frm                  no boot.bin: the installed FSBL/clock stays
#   apps-cpu495-ddr396.frm    + Tezuka FSBL CPU 495 MHz / DDR 396 MT/s
#   apps-cpu950-ddr600.frm    + Tezuka FSBL CPU 950 MHz / DDR 600 MT/s
#   apps-cpu1100-ddr750.frm   + Tezuka FSBL CPU 1100 MHz / DDR 750 MT/s   (labels: tezuka_label)
#   apps-stock-clock.frm      + Tezuka standard-clock FSBL (back from an overclock)
#   inputs: nexus2 (stripped ARMv7 musl ELF), nexus-apps.tar.xz (contrib/pluto/apps: provision.sh,
#   start.sh, defaults/...), svxlink-root.tar.xz (bin lib share var etc/svxlink), SHA256SUMS (all three).
# Payload (unpacked at boot to /opt/apps): nexus/ (nexus-apps verbatim) + nexus/bin/nexus2, svxlink/,
# MANIFEST.txt. boot.bin images: tezuka-boot.sh (pinned by sha256).
set -euo pipefail
IN=$(cd "${1:?inputs}" && pwd); OUT=$(mkdir -p "${2:?out}" && cd "$2" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/tezuka-boot.sh"
SLOT_MAX=$((9 * 1024 * 1024 - 4096))
cd "$IN"
for f in nexus2 nexus-apps.tar.xz svxlink-root.tar.xz SHA256SUMS; do [ -s "$f" ] || { echo "missing input $f"; exit 1; }; done
for f in nexus2 nexus-apps.tar.xz svxlink-root.tar.xz; do grep -q "  $f\$" SHA256SUMS || { echo "SHA256SUMS lacks $f"; exit 1; }; done
grep -E '  (nexus2|nexus-apps\.tar\.xz|svxlink-root\.tar\.xz)$' SHA256SUMS | sha256sum -c
file nexus2 | grep -q 'ELF 32-bit LSB.*ARM' || { echo "nexus2 is not a 32-bit ARM ELF"; exit 1; }
S=$(mktemp -d); trap 'rm -rf "$S"' EXIT
mkdir -p "$S/p/nexus/bin" "$S/p/svxlink"
tar -xJf nexus-apps.tar.xz -C "$S/p/nexus" --no-same-owner
[ -f "$S/p/nexus/provision.sh" ] && [ -f "$S/p/nexus/start.sh" ] || { echo "nexus-apps.tar.xz must hold provision.sh and start.sh at its top level"; exit 1; }
chmod 0755 "$S/p/nexus/provision.sh" "$S/p/nexus/start.sh"
install -m 0755 nexus2 "$S/p/nexus/bin/nexus2"
tar -xJf svxlink-root.tar.xz -C "$S/p/svxlink" --no-same-owner
[ -x "$S/p/svxlink/bin/svxlink" ] || { echo "svxlink-root.tar.xz must hold bin/svxlink at its top level"; exit 1; }
for x in "$S/p/svxlink"/*; do case "${x##*/}" in bin|lib|share|var|etc) ;; *) echo "unexpected ${x##*/} in svxlink-root"; exit 1 ;; esac; done
# defensive cleaning of the svxlink template (the uploader sanitizes too)
C=$S/p/svxlink/etc/svxlink
if [ -f "$C/svxlink.conf" ]; then
  sed -i -e 's/^AUTH_KEY=.*/AUTH_KEY=CHANGE_ME/' -e 's/^CALLSIGN=[^-]*-\(.*\)$/CALLSIGN=N0CALL-\1/' -e 's/^CALLSIGN=[^-]*$/CALLSIGN=N0CALL/' "$C/svxlink.conf"
fi
find "$S/p" \( -name '._*' -o -name .DS_Store \) -delete
# no credential may be shipped: every secret-looking key=value must be a placeholder
python3 -I - "$S/p" <<'PY' || { echo "refusing: credential-like values in the payload (above)"; exit 1; }
import os, re, sys
pat = re.compile(r'(?i)^\s*(auth_?key|password|passwd|pass|api_?key|token|secret|psk|pwd_wlan|ssid_wlan)\s*=\s*(.*)$')
ok_vals = {'', 'change_me', 'changeme', '...'}
bad = 0
for root, _, files in os.walk(sys.argv[1]):
    for f in files:
        fp = os.path.join(root, f)
        try: data = open(fp, 'rb').read()
        except OSError: continue
        if b'\0' in data[:4096]: continue
        for n, line in enumerate(data.decode('utf-8', 'replace').splitlines(), 1):
            m = pat.match(line)
            if not m: continue
            v = m.group(2).split('#')[0].strip().strip('"\'').strip()
            if v.lower() in ok_vals or v.startswith(('$', '<')): continue
            print("%s:%d: %s=<value>" % (os.path.relpath(fp, sys.argv[1]), n, m.group(1))); bad += 1
sys.exit(1 if bad else 0)
PY
! grep -rIl -E 'NEXUS2_BRANDMEISTER_API_KEY=[^$"[:space:]]' "$S/p" || { echo "refusing: BrandMeister API key in the payload"; exit 1; }
{ echo "NexusBS application slot"
  echo "created $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "inputs ${APPS_INPUTS_TAG:-local}"
  echo "firmware-repo $(git -C "$HERE" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  sed 's/^/input /' SHA256SUMS
} > "$S/p/MANIFEST.txt"
long=$(cd "$S/p" && find . | awk 'length($0) > 250'); [ -z "$long" ] || { echo "path too long for ustar: $long"; exit 1; }
( cd "$S/p" && tar --format=ustar --owner=0 --group=0 --numeric-owner --sort=name -cf - . ) | xz -9e --check=crc32 -c > "$S/payload"
PS=$(stat -c %s "$S/payload"); [ "$PS" -le "$SLOT_MAX" ] || { echo "payload $PS > slot $SLOT_MAX"; exit 1; }
PM=$(md5sum "$S/payload" | cut -d' ' -f1)
assemble() {   # assemble <out name> <boot.bin file or ""> <description>
	local n=$1 bb=$2 bd=$3 BS=0 BM= F SIZE
	[ -n "$bb" ] && { BS=$(stat -c %s "$bb"); BM=$(md5sum "$bb" | cut -d' ' -f1); }
	python3 -I - "$S/payload" "$PS" "$PM" "$BS" "$BM" "$bd" "apps-${APPS_INPUTS_TAG:-local}-$(date -u +%Y%m%d)" "$bb" "$OUT/$n" <<'PY'
import sys, hashlib, time
pl, ps, pm, bs, bm, bd, ver, bb, outp = sys.argv[1:]
hdr = ("NEXUSBS-APPS-FRM 1\npayload_size=%s\npayload_md5=%s\nbootbin_size=%s\nbootbin_md5=%s\n"
       "bootbin_desc=%s\nversion=%s\ncreated=%s\n") % (ps, pm, bs, bm, bd, ver, time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()))
hdr = hdr.encode(); assert len(hdr) < 4096; hdr += b'\0' * (4096 - len(hdr))
body = hdr + open(pl, 'rb').read() + (open(bb, 'rb').read() if int(bs) else b'')
open(outp, 'wb').write(body + (hashlib.md5(body).hexdigest() + '\n').encode())
PY
	# self-check exactly as apps-frm-install parses it
	F=$OUT/$n; SIZE=$(stat -c %s "$F")
	[ "$(tail -c 33 "$F" | head -c 32)" = "$(head -c $((SIZE - 33)) "$F" | md5sum | cut -d' ' -f1)" ]
	[ $((4096 + PS + BS + 33)) -eq "$SIZE" ]
	head -c $((4096 + PS)) "$F" | tail -c "$PS" | md5sum | grep -q "^$PM "
	printf '%s: %s bytes, boot.bin %s bytes (%s)\n' "$n" "$SIZE" "$BS" "$bd"
}
xz -t "$S/payload"
assemble apps.frm "" none
for v in cpu495-ddr396 cpu950-ddr600 cpu1100-ddr750 std; do
	tezuka_boot "$v" "$S/boot-$v.bin"
	case $v in std) n=apps-stock-clock.frm ;; *) n=apps-$v.frm ;; esac
	assemble "$n" "$S/boot-$v.bin" "$TZ_DESC"
done
printf 'payload %s of %s slot bytes (%s%%)\n' "$PS" "$SLOT_MAX" "$((PS * 100 / SLOT_MAX))"
