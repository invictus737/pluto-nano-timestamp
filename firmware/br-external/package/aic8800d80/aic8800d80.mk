################################################################################
# aic8800d80: out-of-tree Wi-Fi driver + firmware for the AIC8800D80 USB adapter
################################################################################
AIC8800D80_VERSION = main
AIC8800D80_SITE = $(call github,shenmintao,aic8800d80,$(AIC8800D80_VERSION))
AIC8800D80_LICENSE = GPL-2.0
AIC8800D80_LICENSE_FILES = LICENSE
AIC8800D80_DEPENDENCIES = linux

# The driver's Makefile hardcodes vendor platforms; drive Kbuild directly.
AIC8800D80_MODULE_SUBDIRS = drivers/aic8800/aic_load_fw drivers/aic8800/aic8800_fdrv
AIC8800D80_MODULE_MAKE_OPTS = \
	CONFIG_AIC_LOADFW_SUPPORT=m CONFIG_AIC8800_WLAN_SUPPORT=m \
	CONFIG_PLATFORM_UBUNTU=n CONFIG_AIC_FW_PATH=\"/lib/firmware/aic8800D80\"

define AIC8800D80_INSTALL_TARGET_CMDS
	mkdir -p $(TARGET_DIR)/lib/firmware/aic8800D80
	cp -a $(@D)/fw/aic8800D80/* $(TARGET_DIR)/lib/firmware/aic8800D80/ 2>/dev/null || \
		cp -a $(@D)/fw/aic8800/* $(TARGET_DIR)/lib/firmware/aic8800D80/
	$(INSTALL) -D -m 0644 $(@D)/aic.rules $(TARGET_DIR)/etc/udev/rules.d/90-aic8800.rules 2>/dev/null || true
endef

$(eval $(kernel-module))
$(eval $(generic-package))
