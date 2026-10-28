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

all: $(B)/nexus-rtos.bin

$(B)/main.o: main.c | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/nexus-rtos.elf: $(B)/main.o
	$(CC) $(CFLAGS) $^ -o $@ -Wl,--start-group -lbsd -lm -lz -Wl,--end-group $(LDFLAGS) -Wl,-Map,$(B)/nexus-rtos.map
	$(SIZE) $@

$(B)/nexus-rtos.bin: $(B)/nexus-rtos.elf
	$(OBJCOPY) -O binary $< $@
	@ls -la $@

$(B):
	mkdir -p $@

clean:
	rm -rf $(B)
.PHONY: all clean
