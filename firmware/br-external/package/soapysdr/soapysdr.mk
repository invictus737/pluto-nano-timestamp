################################################################################
# soapysdr: SoapySDR 0.8.1 library and SoapySDRUtil (no Python, no docs)
################################################################################
SOAPYSDR_VERSION = soapy-sdr-0.8.1
SOAPYSDR_SITE = $(call github,pothosware,SoapySDR,$(SOAPYSDR_VERSION))
SOAPYSDR_LICENSE = BSL-1.0
SOAPYSDR_LICENSE_FILES = LICENSE_1_0.txt
SOAPYSDR_INSTALL_STAGING = YES
SOAPYSDR_CONF_OPTS = -DENABLE_PYTHON=OFF -DENABLE_PYTHON3=OFF -DENABLE_DOCS=OFF \
	-DENABLE_TESTS=OFF -DENABLE_APPS=ON

$(eval $(cmake-package))
