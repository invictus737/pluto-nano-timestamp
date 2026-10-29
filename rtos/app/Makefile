# Nexus-BS RTOS base image (M1/M2). Needs the RTEMS prefix with the Pluto BSP and libbsd.
# usage: make -C rtos/app RTEMS_TOOLS=/opt/rtems/6 RTEMS_PREFIX=/opt/rtems/bsp
RTEMS_TOOLS ?= /opt/rtems/6
RTEMS_PREFIX ?= /opt/rtems/bsp
BSP ?= xilinx_zynq_zedboard
PKG := $(RTEMS_PREFIX)/lib/pkgconfig/arm-rtems6-$(BSP).pc
CC := $(RTEMS_TOOLS)/bin/arm-rtems6-gcc
OBJCOPY := $(RTEMS_TOOLS)/bin/arm-rtems6-objcopy
SIZE := $(RTEMS_TOOLS)/bin/arm-rtems6-size
CFLAGS := $(shell PKG_CONFIG_PATH=$(RTEMS_PREFIX)/lib/pkgconfig pkg-config --cflags $(PKG)) -O2 -g -Wall -Wno-unused-result
LDFLAGS := $(shell PKG_CONFIG_PATH=$(RTEMS_PREFIX)/lib/pkgconfig pkg-config --libs $(PKG))
B := build
# Optional Rust static library (M3 probe, later nexus2): adds -DNEXUS_RUST and links it.
RUST_LIB ?=
ifneq ($(RUST_LIB),)
CFLAGS += -DNEXUS_RUST
endif
# Optional SSH server (M2e): wolfSSL + wolfSSH static libraries from rtos/ci/wolfssh.sh. Without
# them ssh_server.c builds a stub (nexus_ssh_start() returns -1) and the image still links.
# ssh_secrets.h (host key, password hash, authorized keys) is generated into $(B) only.
WOLF_PREFIX ?= /opt/rtems/wolf
SSH_DIR := $(abspath $(CURDIR)/../ssh)
ifneq ($(wildcard $(WOLF_PREFIX)/lib/libwolfssh.a),)
SSH_CFLAGS := -DNEXUS_SSH -DWOLFSSL_USER_SETTINGS -I$(SSH_DIR) -I$(WOLF_PREFIX)/include -I$(B)
SSH_LIBS := $(WOLF_PREFIX)/lib/libwolfssh.a $(WOLF_PREFIX)/lib/libwolfssl.a -ltelnetd
SSH_SECRETS := $(B)/ssh_secrets.h
endif

all: $(B)/nexus-rtos.bin

$(B)/main.o: main.c | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/platform.o: platform.c | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

# `abi` shell command: table generated from nexus2 scripts/rtems-abi (rtos/ci/nexus2-abi.sh)
$(B)/abi.o: abi.c abi_table.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/ssh_server.o: ssh_server.c ssh_server.h $(SSH_SECRETS) | $(B)
	$(CC) $(CFLAGS) $(SSH_CFLAGS) -c $< -o $@

# Secrets come from the environment (NEXUS_SSH_HOSTKEY, NEXUS_SSH_PASSWORD), so regenerate
# on every build; the file is private to the build directory.
$(B)/ssh_secrets.h: $(SSH_DIR)/gen_secrets.py $(SSH_DIR)/authorized_keys FORCE | $(B)
	python3 $(SSH_DIR)/gen_secrets.py --authorized-keys $(SSH_DIR)/authorized_keys --out $@

# Persistent configuration (rtos/config/README.md). The Wi-Fi seed header comes from the
# environment (NEXUS_WIFI_SSID, NEXUS_WIFI_PSK) into $(B) only.
CONFIG_OBJS := $(B)/config_store.o $(B)/config_kv.o $(B)/qspi_nor.o
$(B)/nexus_wifi_defaults.h: FORCE | $(B)
	@sh $(CURDIR)/../config/gen-wifi-defaults.sh $@
$(B)/config_store.o: config_store.c config_store.h config_kv.h qspi_nor.h $(B)/nexus_wifi_defaults.h | $(B)
	$(CC) $(CFLAGS) -I$(B) -c $< -o $@
$(B)/config_kv.o: config_kv.c config_kv.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/qspi_nor.o: qspi_nor.c qspi_nor.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

# Network control (netcmd.c): `net` command, static/DHCP per interface, net80211 Wi-Fi
# supervisor, tun. Rides along with the configuration objects (image and libnexusrtos.a).
CONFIG_OBJS += $(B)/netcmd.o
$(B)/netcmd.o: netcmd.c netcmd.h config_store.h ../libbsd/aic8800/aicwf_wifi.h | $(B)
	$(CC) $(CFLAGS) -I../libbsd/aic8800 -c $< -o $@
# Network firmware update (fwupdate.c): FIT over TCP into the QSPI boot slot
CONFIG_OBJS += $(B)/fwupdate.o
$(B)/fwupdate.o: fwupdate.c config_store.h qspi_nor.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/nexus-rtos.elf: $(B)/main.o $(B)/platform.o $(B)/abi.o $(B)/ssh_server.o $(CONFIG_OBJS) $(RUST_LIB)
	$(CC) $(CFLAGS) $(B)/main.o $(B)/platform.o $(B)/abi.o $(B)/ssh_server.o $(CONFIG_OBJS) -o $@ -Wl,--start-group $(RUST_LIB) $(SSH_LIBS) -lbsd -ljffs2 -lm -lz -Wl,--end-group $(LDFLAGS) -Wl,-Map,$(B)/nexus-rtos.map
	$(SIZE) $@

$(B)/nexus-rtos.bin: $(B)/nexus-rtos.elf
	$(OBJCOPY) -O binary $< $@
	@ls -la $@

$(B):
	mkdir -p $@

clean:
	rm -rf $(B)
FORCE:
.PHONY: all clean FORCE
include ../libbsd/aic8800/aicwf.mk
# USB host stack: libbsd (FreeBSD USB, default) or cherry (CherryUSB EHCI host for the AIC8800
# stick, rtos/cherryusb; the libbsd EHCI glue then does not attach)
USB_STACK ?= libbsd
ifeq ($(USB_STACK),cherry)
include ../cherryusb/cherryusb.mk
else ifneq ($(USB_STACK),libbsd)
$(error USB_STACK must be libbsd or cherry, not $(USB_STACK))
endif
# Bluetooth (rtos/bt): BTstack A2DP source; must precede the libnexusrtos.a rule
include ../bt/bt.mk

# nexus2 build (rtos/ci/nexus2-link.sh): the C side as one library, Init starting nexus2's main().
AR := $(RTEMS_TOOLS)/bin/arm-rtems6-ar
$(B)/main_n2.o: main.c | $(B)
	$(CC) $(CFLAGS) -DNEXUS_RUST_MAIN -c $< -o $@
$(B)/posix_compat.o: posix_compat.c nexus_station.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/nexus_load.o: nexus_load.c nexus_station.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/libnexusrtos.a: $(B)/main_n2.o $(B)/posix_compat.o $(B)/nexus_load.o $(B)/platform.o $(B)/abi.o $(B)/ssh_server.o $(CONFIG_OBJS)
	rm -f $@ && $(AR) rcs $@ $^
nexus2lib: $(B)/libnexusrtos.a
.PHONY: nexus2lib
