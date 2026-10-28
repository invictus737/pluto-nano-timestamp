#!/usr/bin/env bash
# Fetch the AIC8800 firmware the RTEMS port embeds and turn it into C (aicwf_fw_data.c).
#
# usage: rtos/libbsd/aic8800/fetch-firmware.sh <out aicwf_fw_data.c>
#   AICWF_FW_LOCAL=<checkout of canmv-k230/aic8800_fw> and AICWF_FW_LEGACY_LOCAL=<checkout of
#   shenmintao/aic8800d80> use local copies instead of downloading.
#
# No firmware is committed to this repository. The files come from the firmware
# repository the RT-Smart driver (rtsmart/SOURCE) is maintained against, at a pinned
# commit, and are checked against the sha256 below. They are vendor binaries under the
# vendor's terms (see that repository's NOTICE), synchronised there from AIC's
# aic8800d_linux_sdk_V5.0_2026_0123_5f7be68d.
#
# Files and why (USB transport only; the driver's aic_firmware_required_files_available()
# and radio configuration name them):
#   AIC8800D80/D40 (a69c:8d80 boot -> 8d81 runtime):
#     fw_patch_table_8800d80_u02.bin   BT/system patch table (adid/patch addresses, MODE...)
#     fw_adid_8800d80_u02.bin          BT ADID image
#     fw_patch_8800d80_u02.bin         BT ROM patch (BT stays usable after the load)
#     fw_patch_8800d80_u02_ext0.bin    external patch named by the table
#     fmacfw_8800d80_u02.bin           Wi-Fi fullmac firmware
#     fmacfw_8800d80_h_u02.bin         same for "H" silicon
#     aic_userconfig_8800d80.txt       per-rate TX power targets, capped at 15 dBm here
#     aic_powerlimit_8800d80.txt       per-country power limits
#   AIC8800DC/DW (a69c:88dc/88dd, ROM firmware + RAM patches):
#   AIC8800 (plain, 0x8800/0x8801): fmacfw.bin, fw_adid*.bin, fw_patch*.bin, fw_patch_table*.bin
#     (u03 variants for the later ROM), aic_userconfig.txt
#     fmacfw_patch_8800dc_u02.bin, fmacfw_patch_8800dc_h_u02.bin        Wi-Fi ROM patch
#     fmacfw_patch_tbl_8800dc_u02.bin, fmacfw_patch_tbl_8800dc_h_u02.bin patch descriptor
#     fmacfw_calib_8800dc_u02.bin, fmacfw_calib_8800dc_h_u02.bin        DPD calibration
#     aic_userconfig_8800dc.txt, aic_userconfig_8800dw.txt              TX power (capped)
#     aic_powerlimit_8800dc.txt, aic_powerlimit_8800dw.txt              country limits
#   AIC8800D80 MCU1 units (chip_mcu_id 1, bit 25 of 0x40500000 clear), published as
#   aic8800D80_mcu1/: the SDK V3 set the Linux image drives them with, from
#   github.com/shenmintao/aic8800d80 (branch legacy-mcu1) at a pinned commit; the SDK 5.0
#   files do not load on these units. The driver picks it when it reads mcu=1
#   (rtsmart/aic8800/aic8800_firmware.c, AICWF_RTEMS):
#     fw_patch_table_8800d80_u02.bin, fw_adid_8800d80_u02.bin, fw_patch_8800d80_u02.bin,
#     fmacfw_8800d80_u02.bin
#   (the runtime keeps the aic8800D80 userconfig/powerlimit, same TX power v3 format).
# Not fetched: SDIO files, AIC8800/8801 and D80X2 families, RF test, IPC and BLE-scan
# images, calibmode, U01/U04 images.
set -euo pipefail
OUT=${1:?output .c file}
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=canmv-k230/aic8800_fw
COMMIT=2a47560abfa473ef7bff32321ef3db94a469d92d
BASE=https://raw.githubusercontent.com/$REPO/$COMMIT

LEGACY_REPO=shenmintao/aic8800d80
LEGACY_COMMIT=a9683a456079131a0381ed75fb7a4f5514527b3c
LEGACY_BASE=https://raw.githubusercontent.com/$LEGACY_REPO/$LEGACY_COMMIT

# path in the legacy repository | published directory | sha256
LEGACY_FILES="
fw/aic8800D80/fw_patch_table_8800d80_u02.bin|aic8800D80_mcu1|58c07eb4c79de6e6beff683686495dec152688f7de59816cfbe71a0c664ca7c8
fw/aic8800D80/fw_adid_8800d80_u02.bin|aic8800D80_mcu1|a526cbd02fcdc495f049f3ad6b5933cb08cd984b16790c716a060d582fee1a56
fw/aic8800D80/fw_patch_8800d80_u02.bin|aic8800D80_mcu1|3c5fa5bb678b835adbe42cfe0326074cf8aa5c6d3b93d16d2943b63ad4106b72
fw/aic8800D80/fmacfw_8800d80_u02.bin|aic8800D80_mcu1|1ec680c2b63dcaa0e5d33c5fb6d1857d030f8145c05c385e243760388a61a0da
"

# path in the repository | sha256
FILES="
usb/aic8800D80/fw_patch_table_8800d80_u02.bin|9decb77435b7e9713e33e32da483d683b7329ed93b672b2d1b134031d7da5f67
usb/aic8800D80/fw_adid_8800d80_u02.bin|a526cbd02fcdc495f049f3ad6b5933cb08cd984b16790c716a060d582fee1a56
usb/aic8800D80/fw_patch_8800d80_u02.bin|f0e2f5bbc17bc327ca7f1574ff55370dfd863d931514347bb4abc18a74f6218f
usb/aic8800D80/fw_patch_8800d80_u02_ext0.bin|ca738f5ea5aca6021c4f48b60ff35e835dfdaf072a8490e3ba8ba0a42440e3e5
usb/aic8800D80/fmacfw_8800d80_u02.bin|23f2bed2ac0e8f7bda8f36df7defd30d9b928c97867f6f0f01f28d89dd78d670
usb/aic8800D80/fmacfw_8800d80_h_u02.bin|c5c8c774dfae93aed71f8da7489e35159192135e4929591ea8a945b76deba6bd
usb/aic8800D80/aic_userconfig_8800d80.txt|56c039d10af63c888b18890f508968a143f034de279fd2c4ec7ee65133e3dbdb
usb/aic8800D80/aic_powerlimit_8800d80.txt|a21645878dd5a065c48eaf4aee36349d214fd43e530a47af23973e5e81b9904a
usb/aic8800DC/fmacfw_patch_8800dc_u02.bin|7df85a59e397fa6c8e15ff9e255e0d7902b4b7939b408066bc5af4ea4d5cee31
usb/aic8800DC/fmacfw_patch_8800dc_h_u02.bin|cd8ff23201cd47248169141ef1f77320750d4123ca4b32387cbf28dc02523c15
usb/aic8800DC/fmacfw_patch_tbl_8800dc_u02.bin|42465d1e755728a3c6b46ac3527c631af4a089c30d691401ea0dc3845ae4a982
usb/aic8800DC/fmacfw_patch_tbl_8800dc_h_u02.bin|d4c2f0f9559c71fb5765471aa5cdd804512931c02699a23551c621c2325817e3
usb/aic8800DC/fmacfw_calib_8800dc_u02.bin|1e485503e232c83d2149d7ec80563ed391b3cca9370cfabda40c9f06744e0daa
usb/aic8800DC/fmacfw_calib_8800dc_h_u02.bin|edb130990c1531d71ac1fca037d7790e7844cd14e985d277f9c232841ea2b7ae
usb/aic8800DC/aic_userconfig_8800dc.txt|1d51362568e82cb087dd2ce6414063b367558392fc8567d9c10e823fd2ee4bc6
usb/aic8800DC/aic_userconfig_8800dw.txt|1d51362568e82cb087dd2ce6414063b367558392fc8567d9c10e823fd2ee4bc6
usb/aic8800DC/aic_powerlimit_8800dc.txt|a21645878dd5a065c48eaf4aee36349d214fd43e530a47af23973e5e81b9904a
usb/aic8800DC/aic_powerlimit_8800dw.txt|a21645878dd5a065c48eaf4aee36349d214fd43e530a47af23973e5e81b9904a
usb/aic8800/fmacfw.bin|a5d142031150b33771ec80772b1e7c96d57a1c7dccc8151a4f729408d7bd0f57
usb/aic8800/fw_adid.bin|1a54cd5f56552b1df20ae20377194ea1e0b1c6e65df8873c3d6a5ab0f4760828
usb/aic8800/fw_adid_u03.bin|6c7cc9d899d2a4e5b91b0f009aa6679498131adc27d220b96ec162536370a190
usb/aic8800/fw_patch.bin|d9e164c90fd76f767bb7f5d5041a1e0f7b69096d7a408ff605920f909207ee14
usb/aic8800/fw_patch_u03.bin|5f024e0b80802a30797d5afb3d06e3a8d02efe74d7ed0f7d4ee807093a579067
usb/aic8800/fw_patch_table.bin|38d572126d767d2fe34b011076675195ddcb108c67dbaca0c14dbc9dc9617964
usb/aic8800/fw_patch_table_u03.bin|0b1f6c1f1c845e3b0665a1bd1a5cd433d55bd24b79a86bbb6b06c8446453e1a1
usb/aic8800/aic_userconfig.txt|2940418e3a83d0b718618213572bcd77d54b561d83da7057d40274139321952b
"

sha256() {
	if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
	else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
LIST=()
for entry in $FILES; do
	path=${entry%%|*}; want=${entry##*|}
	mkdir -p "$TMP/$(dirname "$path")"
	if [ -n "${AICWF_FW_LOCAL:-}" ]; then
		cp "$AICWF_FW_LOCAL/$path" "$TMP/$path"
	else
		curl -sfL --retry 3 -o "$TMP/$path" "$BASE/$path"
	fi
	got=$(sha256 "$TMP/$path")
	if [ "$got" != "$want" ]; then
		echo "aicwf: sha256 mismatch for $path: $got (expected $want)" >&2
		exit 1
	fi
	LIST+=("$path")
done
for entry in $LEGACY_FILES; do
	path=${entry%%|*}; rest=${entry#*|}; dest=${rest%%|*}; want=${rest##*|}
	out="legacy/$dest/$(basename "$path")"
	mkdir -p "$TMP/legacy/$dest"
	if [ -n "${AICWF_FW_LEGACY_LOCAL:-}" ]; then
		cp "$AICWF_FW_LEGACY_LOCAL/$path" "$TMP/$out"
	else
		curl -sfL --retry 3 -o "$TMP/$out" "$LEGACY_BASE/$path"
	fi
	got=$(sha256 "$TMP/$out")
	if [ "$got" != "$want" ]; then
		echo "aicwf: sha256 mismatch for $LEGACY_REPO $path: $got (expected $want)" >&2
		exit 1
	fi
	LIST+=("$out")
done
python3 -I "$HERE/gen_fwdata.py" --src "$TMP" --out "$OUT" "${LIST[@]}"
