#!/usr/bin/env bash
# ADI no-OS (pinned release v1.251.0, sha256 checked): the AD9361/AD9363 driver
# (drivers/rf-transceiver/ad9361), the axi_adc_core / axi_dac_core / axi_dmac cores and the
# minimal no_os_* utility, allocation, delay, SPI and GPIO interfaces the driver needs. Only that
# subset is extracted. The native Pluto driver (rtos/sdr/pluto_rtems) provides the RTEMS platform
# layer (platform_*.c) and its own AXI DMAC streaming engine.
# usage: rtos/sdr/fetch-noos.sh <destination dir>   (creates <dest>/no-OS)
set -euo pipefail
DEST=${1:?destination}
VER=1.251.0
SHA=cfa795a0f29b7a4d82e3b8ba349690353753752f37a053f3391418735a9b05fd
URL="https://github.com/analogdevicesinc/no-OS/archive/refs/tags/v$VER.tar.gz"
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
curl -sfL --retry 3 -o "$W/noos.tar.gz" "$URL"
GOT=$( (sha256sum "$W/noos.tar.gz" 2>/dev/null || shasum -a 256 "$W/noos.tar.gz") | cut -c1-64)
echo "no-OS v$VER sha256 $GOT"
[ "${NOOS_SHA_CHECK:-1}" = 0 ] || [ "$GOT" = "$SHA" ] || { echo "no-OS tarball hash mismatch"; exit 1; }
P=no-OS-$VER
tar xzf "$W/noos.tar.gz" -C "$W" \
  "$P/drivers/rf-transceiver/ad9361" \
  "$P/drivers/axi_core/axi_adc_core" "$P/drivers/axi_core/axi_dac_core" "$P/drivers/axi_core/axi_dmac" \
  "$P/drivers/api/no_os_spi.c" "$P/drivers/api/no_os_gpio.c" \
  "$P/util/no_os_util.c" "$P/util/no_os_alloc.c" "$P/util/no_os_mutex.c" \
  "$P/include" "$P/LICENSE"
rm -rf "$DEST/no-OS"
mkdir -p "$DEST"
mv "$W/$P" "$DEST/no-OS"
echo "$VER $SHA" > "$DEST/no-OS/VERSION.nexus"
ls "$DEST/no-OS/drivers/rf-transceiver/ad9361"
