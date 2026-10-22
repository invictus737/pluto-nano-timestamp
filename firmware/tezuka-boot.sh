# Tezuka v0.3.24 Nano boot images (FSBL + the same U-Boot), pinned; sourced by build-boot-frm.sh and
# build-apps-frm.sh. tezuka_boot <variant> <dest file>   variant: std | cpu495-ddr396 | cpu950-ddr600 | cpu1100-ddr750
TZ_ZIP=tezuka-nano-v0.3.24-a304a1f.zip
TZ_URL=https://github.com/F5OEO/tezuka_fw/releases/download/v0.3.24/$TZ_ZIP
TZ_ZIP_SHA=6ddc4af05f07e892cbcc9b08f6a9831f1130706962028c8c83a4587e4e8f7d36
TZ_VARIANTS="std cpu495-ddr396 cpu950-ddr600 cpu1100-ddr750"
tezuka_boot() {
	local v=$1 dst=$2 w=${TZ_WORK:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/work-tezuka} m sha desc
	case "$v" in
	std)            m=sdimg/BOOT.bin;                          sha=6ae8d83237b6774280839942e0a8cc1a8f2ef5246e158aa5aff3db15aabec479; desc="Tezuka v0.3.24 Nano BOOT.bin (standard clocks)" ;;
	cpu495-ddr396)  m=sdimg/overclock/BOOT_fsbl_CPU495_DDR396;  sha=59ca20b3724af358765574fadc13dd13f973887ed86a629c70fb78202b7f8f6b; desc="Tezuka v0.3.24 Nano BOOT_fsbl_CPU495_DDR396" ;;
	cpu950-ddr600)  m=sdimg/overclock/BOOT_fsbl_CPU950_DDR600;  sha=e0a9037dc7b5a636263e13286b783ab67ccc452b53aad699915dadf0d314512e; desc="Tezuka v0.3.24 Nano BOOT_fsbl_CPU950_DDR600" ;;
	cpu1100-ddr750) m=sdimg/overclock/BOOT_fsbl_CPU1100_DDR750; sha=d803035aece5cf1efca1a9c779b8629283929615515d31fe6888f71ec0165980; desc="Tezuka v0.3.24 Nano BOOT_fsbl_CPU1100_DDR750" ;;
	*) echo "unknown boot variant $v" >&2; return 1 ;;
	esac
	mkdir -p "$w"
	echo "$TZ_ZIP_SHA  $w/$TZ_ZIP" | sha256sum -c --status 2>/dev/null || curl -sfL -o "$w/$TZ_ZIP" "$TZ_URL"
	echo "$TZ_ZIP_SHA  $w/$TZ_ZIP" | sha256sum -c --status || { echo "Tezuka release checksum mismatch" >&2; return 1; }
	unzip -p "$w/$TZ_ZIP" "${TZ_ZIP%.zip}/$m" > "$dst"
	echo "$sha  $dst" | sha256sum -c --status || { echo "$v: boot image checksum mismatch" >&2; return 1; }
	TZ_DESC=$desc
}
# release-table label of a variant (tezuka_label <variant>)
tezuka_label() {
	case "$1" in
	std)            echo "standard clocks (Tezuka default)" ;;
	cpu495-ddr396)  echo "CPU 495 MHz / DDR 396 MT/s: Tezuka file, not tested on the reference board" ;;
	cpu950-ddr600)  echo "CPU 950 MHz / DDR 600 MT/s: Tezuka file, not tested on the reference board" ;;
	cpu1100-ddr750) echo "CPU 1100 MHz / DDR 750 MT/s: tested on the reference board (HamGeek Pluto Nano Rev.B) with heatsinks and a fan installed, ran for days; above the -1 speed grade spec, active cooling required" ;;
	esac
}
# config.txt cpu_clock value of a variant (tezuka_setting <variant>)
tezuka_setting() { case "$1" in std) echo standard ;; cpu495-ddr396) echo 495_396 ;; cpu950-ddr600) echo 950_600 ;; cpu1100-ddr750) echo 1100_750 ;; esac; }
