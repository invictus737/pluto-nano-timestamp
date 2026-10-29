# Host unit tests and compile checks for the native Pluto driver (rtos/sdr/pluto_rtems).
#   make                 framing + ring tests (host cc)
#   make SOAPY_SRC=...   also checks the tick/ns math against SoapySDR 0.8.1 lib/TimeC.cpp
#   make NOOS=<fetch-noos.sh dest>/no-OS   also runs the patched no-OS ad9361 init/rate test
#   make check-rtems CROSS=aarch64-linux-gnu- NOOS=... SOAPY_SRC=...
#                        type checks the RTEMS-only sources (driver, DMA, platform) with a Linux
#                        cross compiler and the API shims in shim/ (compile only, no link)
SRC := ../pluto_rtems
CC ?= cc
CXX ?= c++
CFLAGS += -std=c11 -O2 -g -Wall -Wextra -Werror -I$(SRC)
B := build
SOAPY_SRC ?=
NOOS ?=
CROSS ?= aarch64-linux-gnu-

TESTS := $(B)/test_framing $(B)/test_ring $(B)/test_probe
ifneq ($(SOAPY_SRC),)
TESTS += $(B)/test_time_soapy
endif
ifneq ($(NOOS),)
TESTS += $(B)/test_noos_init
endif

all: run

$(B):
	mkdir -p $@

$(B)/test_framing: test_framing.c $(SRC)/pluto_framing.c $(SRC)/pluto_framing.h | $(B)
	$(CC) $(CFLAGS) test_framing.c $(SRC)/pluto_framing.c -lm -o $@

$(B)/test_ring: test_ring.c $(SRC)/pluto_ring.c $(SRC)/pluto_ring.h | $(B)
	$(CC) $(CFLAGS) -pthread test_ring.c $(SRC)/pluto_ring.c -o $@

$(B)/test_probe: test_probe.c $(SRC)/pluto_probe.c $(SRC)/pluto_probe.h $(SRC)/pluto_framing.c | $(B)
	$(CC) $(CFLAGS) test_probe.c $(SRC)/pluto_probe.c $(SRC)/pluto_framing.c -lm -o $@

# the no-OS ad9361 driver (fetch-noos.sh tree, patches applied) against a register-file chip model
NOOS_SRCS = drivers/rf-transceiver/ad9361/ad9361.c drivers/rf-transceiver/ad9361/ad9361_api.c \
  drivers/rf-transceiver/ad9361/ad9361_conv.c drivers/rf-transceiver/ad9361/ad9361_util.c \
  drivers/axi_core/axi_adc_core/axi_adc_core.c drivers/axi_core/axi_dac_core/axi_dac_core.c \
  util/no_os_util.c util/no_os_mutex.c drivers/api/no_os_spi.c drivers/api/no_os_gpio.c
$(B)/test_noos_init: test_noos_init.c $(SRC)/pluto_probe.c | $(B)
	$(CC) -std=gnu11 -O1 -g -w -I$(SRC) $(NOOS_INC) test_noos_init.c $(SRC)/pluto_probe.c \
	  $(addprefix $(NOOS)/,$(NOOS_SRCS)) -lm -o $@

$(B)/test_time_soapy: test_time_soapy.cpp $(SRC)/pluto_framing.c | $(B)
	$(CC) $(CFLAGS) -c $(SRC)/pluto_framing.c -o $(B)/pluto_framing.o
	$(CXX) -std=c++14 -O2 -I$(SRC) -I$(SOAPY_SRC)/include -DSOAPY_SDR_DLL=0 test_time_soapy.cpp \
	  $(SOAPY_SRC)/lib/TimeC.cpp $(B)/pluto_framing.o -o $@

run: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

# Compile-only check of the RTEMS sources: catches API/type errors without an RTEMS toolchain.
RTEMS_C := pluto_dma.c pluto_rt.c pluto_hw.c pluto_probe.c platform_spi.c platform_gpio.c platform_misc.c pluto_ring.c pluto_framing.c
NOOS_INC := -I$(NOOS)/include -I$(NOOS)/drivers/rf-transceiver/ad9361 -I$(NOOS)/drivers/axi_core/axi_adc_core \
            -I$(NOOS)/drivers/axi_core/axi_dac_core -I$(NOOS)/drivers/axi_core/axi_dmac
check-rtems: | $(B)
	@test -n "$(NOOS)" -a -n "$(SOAPY_SRC)" || { echo "set NOOS=<no-OS tree> SOAPY_SRC=<SoapySDR tree>"; exit 1; }
	@for f in $(RTEMS_C); do \
	  $(CROSS)gcc -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Werror -D_GNU_SOURCE -Ishim -I$(SRC) $(NOOS_INC) \
	    -c $(SRC)/$$f -o $(B)/chk_$${f%.c}.o || exit 1; echo "ok $$f"; done
	$(CROSS)g++ -std=c++14 -O2 -Wall -Wextra -Wno-unused-parameter -Werror -Ishim -I$(SRC) -I$(SOAPY_SRC)/include \
	  -c $(SRC)/SoapyPlutoRtems.cpp -o $(B)/chk_SoapyPlutoRtems.o && echo "ok SoapyPlutoRtems.cpp"

clean:
	rm -rf $(B)
.PHONY: all run clean check-rtems
