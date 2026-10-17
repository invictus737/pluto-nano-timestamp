################################################################################
# sdr-ip-gadget: UDP IIO streaming daemon (the commit pgreenland's Pluto+ firmware uses)
################################################################################
SDR_IP_GADGET_VERSION = 9b3864db0898e9e5c38659d45cc02babb5548541
SDR_IP_GADGET_SITE = $(call github,pgreenland,pluto-sdr-ip-gadget,$(SDR_IP_GADGET_VERSION))
SDR_IP_GADGET_LICENSE = MIT
SDR_IP_GADGET_LICENSE_FILES = LICENSE
SDR_IP_GADGET_DEPENDENCIES = libiio
SDR_IP_GADGET_CONF_OPTS = -DGIT_VERSION_OVERRIDE=$(SDR_IP_GADGET_VERSION) -DGENERATE_STATS=OFF

$(eval $(cmake-package))
