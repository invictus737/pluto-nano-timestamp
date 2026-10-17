################################################################################
# soapyplutosdr-ts: SoapyPlutoSDR, pgreenland sdr_gadget_timestamping branch
################################################################################
# 2bbf771 = the commit BlueStation documents for timestamp firmware v0.38
SOAPYPLUTOSDR_TS_VERSION = 2bbf77152d4e6d30c6630807fdfc8a869a528cf3
SOAPYPLUTOSDR_TS_SITE = $(call github,pgreenland,SoapyPlutoSDR,$(SOAPYPLUTOSDR_TS_VERSION))
SOAPYPLUTOSDR_TS_LICENSE = LGPL-2.1
SOAPYPLUTOSDR_TS_LICENSE_FILES = LICENSE
SOAPYPLUTOSDR_TS_DEPENDENCIES = host-pkgconf soapysdr libiio libad9361-iio libusb

$(eval $(cmake-package))
