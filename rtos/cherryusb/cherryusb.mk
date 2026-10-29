# CherryUSB host (USB_STACK=cherry), included by rtos/app/Makefile after aicwf.mk.
#
# Builds the CherryUSB host core, hub class and EHCI port (fetched by fetch-cherryusb.sh, not
# vendored), the RTEMS OSAL, the Zynq USB0 glue, and the AIC8800 adapter that replaces the
# libbsd usbh shim: the RT-Smart driver objects of aicwf.mk are recompiled against CherryUSB
# (compat usbh_core.h first on their include path) and shim/usbh_shim.c leaves the link.
# The libbsd EHCI glue does not attach in this build (main.c, NEXUS_USB_CHERRY), so libbsd has
# no USB bus: no umass/config drop-in stick or net80211 sticks in this image. USB Ethernet comes
# from CherryUSB's own class drivers (RTL8152, CDC-ECM, CDC-NCM, ASIX) glued to libbsd ifnets
# ueN by net/usbnet.c (CHERRY_USBNET=1, default; 0 leaves them out).
CHERRY_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
CHERRY_SRC ?= $(CHERRY_DIR)/CherryUSB

ifeq ($(wildcard $(CHERRY_SRC)/core/usbh_core.c),)
$(error cherryusb: no CherryUSB at $(CHERRY_SRC): run rtos/cherryusb/fetch-cherryusb.sh)
endif

# Host controller driver: CHERRY_HCD=nxp (port/ehci_nxp: NXP MCUXpresso pipe model, one QH per
# endpoint kept linked, cancel by the schedule handshake, no IAA) or cherry (CherryUSB's EHCI
# port with patches/). Both provide usb_hc_ehci.h for the glue and the AIC adapter; the nxp
# directory comes first on the include path so its header wins (usb_ehci_reg.h stays CherryUSB's).
CHERRY_HCD ?= nxp
ifeq ($(CHERRY_HCD),nxp)
CHERRY_HCD_INC := -I$(CHERRY_DIR)/port/ehci_nxp
CHERRY_HCD_OBJ := usb_hc_ehci_nxp.o
else ifeq ($(CHERRY_HCD),cherry)
CHERRY_HCD_INC :=
CHERRY_HCD_OBJ := usb_hc_ehci.o
else
$(error cherryusb: CHERRY_HCD must be nxp or cherry, not $(CHERRY_HCD))
endif

CHERRY_INC := -I$(CHERRY_DIR)/config -I$(CHERRY_DIR)/port $(CHERRY_HCD_INC) -I$(CHERRY_SRC)/common \
	-I$(CHERRY_SRC)/core -I$(CHERRY_SRC)/class/hub -I$(CHERRY_SRC)/class/msc \
	-I$(CHERRY_SRC)/port/ehci
# third-party code: its own warnings are not ours to fix
CHERRY_CFLAGS := $(CFLAGS) -std=gnu11 -Wno-unused-function -Wno-unused-variable \
	-Wno-unused-but-set-variable $(CHERRY_INC)

CFLAGS += -DNEXUS_USB_CHERRY -I$(CHERRY_DIR)/port
$(B)/main.o $(B)/main_n2.o: $(CHERRY_DIR)/port/nexus_cherryusb.h

# the RT-Smart driver, offload framework and shim objects: CherryUSB API instead of the libbsd
# one (aic/include/usbh_core.h, then the real core header through #include_next)
AICWF_CFLAGS := $(CFLAGS) -std=gnu11 -Wno-unused-function -Wno-format \
	-I$(CHERRY_DIR)/aic/include $(CHERRY_INC) \
	-I$(AICWF_DIR)/shim -I$(AICWF_RTS)/aic8800 -I$(AICWF_RTS)/aic8800/port \
	-I$(AICWF_RTS)/wlan -I$(AICWF_RTS)/wlan/offload \
	-I$(AICWF_RTS)/wlan/offload/third_party/tinycrypt/include -I$(AICWF_DIR)

CHERRY_OBJS := $(addprefix $(B)/cherry_, usbh_core.o usbh_hub.o $(CHERRY_HCD_OBJ) usb_osal_rtems.o \
	usb_glue_zynq.o aicwf_cherry_classes.o aicwf_cherry_modeswitch.o aicwf_cherry.o)

# ---- USB Ethernet (net/): CherryUSB class drivers + libbsd ifnet glue ----
# The class table moves to net/usbnet_classes.c (hub, Ethernet classes, then the AIC adapter),
# so aicwf_cherry_classes.o leaves the link; the glue uses aicwf_ifnet_* from libbsd.a.
CHERRY_USBNET ?= 1
ifeq ($(CHERRY_USBNET),1)
CHERRY_CFLAGS += -I$(CHERRY_SRC)/class/cdc -I$(CHERRY_SRC)/class/vendor/net
CFLAGS += -DNEXUS_USBNET -I$(CHERRY_DIR)/net
CHERRY_OBJS := $(filter-out $(B)/cherry_aicwf_cherry_classes.o,$(CHERRY_OBJS)) \
	$(addprefix $(B)/cherry_, usbh_rtl8152.o usbh_asix.o usbh_cdc_ecm.o usbh_cdc_ncm.o usbnet.o usbnet_classes.o)
$(B)/netcmd.o: $(CHERRY_DIR)/net/usbnet.h
else ifneq ($(CHERRY_USBNET),0)
$(error cherryusb: CHERRY_USBNET must be 0 or 1, not $(CHERRY_USBNET))
endif

# usbh_shim.c is replaced: out of the link, and its prerequisite slot (already on the image
# rule from aicwf.mk) becomes an empty object
CONFIG_OBJS := $(filter-out $(B)/aicwf_usbh_shim.o,$(CONFIG_OBJS)) $(CHERRY_OBJS)
$(B)/nexus-rtos.elf: $(CHERRY_OBJS)
# a different CHERRY_HCD rebuilds every object that includes usb_hc_ehci.h
$(B)/cherry_hcd.$(CHERRY_HCD): | $(B)
	rm -f $(B)/cherry_hcd.*
	touch $@
$(CHERRY_OBJS): $(B)/cherry_hcd.$(CHERRY_HCD)

$(B)/aicwf_usbh_shim.o: | $(B)
	echo '/* USB_STACK=cherry: shim/usbh_shim.c is replaced by rtos/cherryusb/aic */' > $(B)/aicwf_usbh_shim_unused.c
	$(CC) $(CFLAGS) -c $(B)/aicwf_usbh_shim_unused.c -o $@

$(B)/cherry_%.o: $(CHERRY_SRC)/core/%.c $(CHERRY_DIR)/config/usb_config.h | $(B)
	$(CC) $(CHERRY_CFLAGS) -c $< -o $@
$(B)/cherry_%.o: $(CHERRY_SRC)/class/hub/%.c $(CHERRY_DIR)/config/usb_config.h | $(B)
	$(CC) $(CHERRY_CFLAGS) -c $< -o $@
$(B)/cherry_%.o: $(CHERRY_SRC)/class/cdc/%.c $(CHERRY_DIR)/config/usb_config.h | $(B)
	$(CC) $(CHERRY_CFLAGS) -c $< -o $@
$(B)/cherry_%.o: $(CHERRY_SRC)/class/vendor/net/%.c $(CHERRY_DIR)/config/usb_config.h | $(B)
	$(CC) $(CHERRY_CFLAGS) -c $< -o $@
$(B)/cherry_usbnet.o: $(CHERRY_DIR)/net/usbnet.c $(CHERRY_DIR)/net/usbnet.h $(AICWF_DIR)/aicwf_usbif.h $(CHERRY_DIR)/config/usb_config.h | $(B)
	$(CC) $(CHERRY_CFLAGS) -I$(AICWF_DIR) -c $< -o $@
$(B)/cherry_usbnet_classes.o: $(CHERRY_DIR)/net/usbnet_classes.c $(CHERRY_DIR)/config/usb_config.h | $(B)
	$(CC) $(CHERRY_CFLAGS) -c $< -o $@
$(B)/cherry_%.o: $(CHERRY_SRC)/port/ehci/%.c $(CHERRY_DIR)/config/usb_config.h | $(B)
	$(CC) $(CHERRY_CFLAGS) -c $< -o $@
$(B)/cherry_%.o: $(CHERRY_DIR)/port/ehci_nxp/%.c $(CHERRY_DIR)/port/ehci_nxp/usb_hc_ehci.h $(CHERRY_DIR)/config/usb_config.h | $(B)
	$(CC) $(CHERRY_CFLAGS) -c $< -o $@
$(B)/cherry_%.o: $(CHERRY_DIR)/osal/%.c $(CHERRY_DIR)/config/usb_config.h | $(B)
	$(CC) $(CHERRY_CFLAGS) -c $< -o $@
$(B)/cherry_%.o: $(CHERRY_DIR)/port/%.c $(CHERRY_DIR)/port/nexus_cherryusb.h $(CHERRY_DIR)/config/usb_config.h | $(B)
	$(CC) $(CHERRY_CFLAGS) -c $< -o $@
$(B)/cherry_aicwf_cherry_classes.o $(B)/cherry_aicwf_cherry_modeswitch.o: $(B)/cherry_%.o: $(CHERRY_DIR)/aic/%.c $(CHERRY_DIR)/config/usb_config.h | $(B)
	$(CC) $(CHERRY_CFLAGS) -c $< -o $@
# the adapter is built like the driver (RT-Thread shim headers, compat usbh_core.h)
$(B)/cherry_aicwf_cherry.o: $(CHERRY_DIR)/aic/aicwf_cherry.c $(CHERRY_DIR)/aic/include/usbh_core.h $(CHERRY_DIR)/config/usb_config.h | $(B)
	$(CC) $(AICWF_CFLAGS) -c $< -o $@
