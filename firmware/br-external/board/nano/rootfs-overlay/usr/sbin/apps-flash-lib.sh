# shell library for the application slot (qspi-apps) and the apps.frm handler; sourced, busybox sh
# Slot layout: 4096-byte text header (NUL padded) + payload (xz-compressed tar), at the start of qspi-apps.
APPS_MAGIC="NEXUSBS-APPS-FRM 1"
# NEXUS_ROOT (tests only) prefixes /proc/mtd and /dev/mtdN
mtdnum() { grep "\"$1\"" ${NEXUS_ROOT:-}/proc/mtd | cut -d: -f1 | tr -dc 0-9; }
mtdsize() { printf '%d' "0x$(grep "\"$1\"" ${NEXUS_ROOT:-}/proc/mtd | awk '{print $2}')"; }
# erase block size of mtdN in bytes
mtderasesize() { printf '%d' "0x$(grep "^mtd$1:" ${NEXUS_ROOT:-}/proc/mtd | awk '{print $3}')"; }
mtddev() { echo "${NEXUS_ROOT:-}/dev/mtd$1"; }
hdrval() { sed -n "s/^$2=//p" "$1" | head -1; }
blkmd5() { dd if="$(mtddev $1)" bs=65536 skip="$2" count="${3:-1}" 2>/dev/null | md5sum | cut -d' ' -f1; }
# Linux addresses the upper 16 MiB bank only with the Winbond EAR patch: refuse when the bank register
# is not readable, or when the start of the slot reads exactly like its lower-bank mirror (offset
# -16 MiB: U-Boot environment + nvmfs), which is what a wrapped (aliased) access returns.
upper_bank_ok() {
	dmesg | grep -q "failed to read ear reg" && { echo "flash bank register unreadable"; return 1; }
	A=$(mtdnum qspi-apps); E=$(mtdnum qspi-uboot-env); N=$(mtdnum qspi-nvmfs)
	[ -n "$A" ] && [ -n "$E" ] && [ -n "$N" ] || { echo "partitions missing"; return 1; }
	same=0; FF=41d2e2c0c0edfccf76fa1c3e38bc1cf2  # md5 of an erased 128 KiB range (131072 x 0xFF)
	a=$(blkmd5 $A 0 2); [ "$a" = "$(blkmd5 $E 0 2)" ] && same=$((same+1))
	b=$(blkmd5 $A 2 2); [ "$b" = "$(blkmd5 $N 0 2)" ] && [ "$b" != $FF ] && same=$((same+1))
	[ $same -eq 0 ] || { echo "qspi-apps reads like its lower-bank mirror (aliased)"; return 1; }
	return 0
}
