# AIC8800 Wi-Fi/Bluetooth (RT-Smart driver on RTEMS), included by rtos/app/Makefile.
# The libbsd part (if_aicwf_usb.c) is built into libbsd.a by rtos/libbsd/apply.sh; this
# builds the RT-Smart driver, the WLAN offload framework, the RTEMS shim, the embedded
# firmware and rtos/app/wifi.c, and links them into the image.
AICWF_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
AICWF_RTS := $(AICWF_DIR)/rtsmart
# generated in CI by rtos/libbsd/aic8800/fetch-firmware.sh (rtos/ci/bsp.sh installs it)
AICWF_FW_DATA ?= $(RTEMS_PREFIX)/share/aicwf/aicwf_fw_data.c

AICWF_CFLAGS := $(CFLAGS) -std=gnu11 -Wno-unused-function -Wno-format \
	-I$(AICWF_DIR)/shim -I$(AICWF_RTS)/aic8800 -I$(AICWF_RTS)/aic8800/port \
	-I$(AICWF_RTS)/wlan -I$(AICWF_RTS)/wlan/offload \
	-I$(AICWF_RTS)/wlan/offload/third_party/tinycrypt/include -I$(AICWF_RTS)/cherryusb \
	-I$(AICWF_DIR)

# objects live in $(B) itself (aicwf_*.o, wifi.o) like the other application objects
AICWF_OBJS := $(addprefix $(B)/aicwf_, \
	aic8800_wifi.o aic8800_firmware.o aic8800_radio.o aic8800_usb.o aic8800_tx_queue.o \
	aic8800_btusb.o \
	wlan_offload.o wlan_offload_bus.o wlan_offload_command.o wlan_offload_crypto.o \
	wlan_offload_sae.o wlan_offload_supplicant.o ecc.o \
	rtt_shim.o usbh_shim.o wlan_dev_shim.o bt_hci_shim.o fw_files.o fw_data.o) \
	$(B)/wifi.o

# the image link line names CONFIG_OBJS: the driver objects ride along with them
CONFIG_OBJS += $(AICWF_OBJS)
$(B)/nexus-rtos.elf: $(AICWF_OBJS)

$(B)/aicwf_%.o: $(AICWF_RTS)/aic8800/%.c | $(B)
	$(CC) $(AICWF_CFLAGS) -c $< -o $@

$(B)/aicwf_%.o: $(AICWF_RTS)/wlan/offload/%.c | $(B)
	$(CC) $(AICWF_CFLAGS) -c $< -o $@

$(B)/aicwf_%.o: $(AICWF_RTS)/wlan/offload/third_party/tinycrypt/%.c | $(B)
	$(CC) $(AICWF_CFLAGS) -c $< -o $@

$(B)/aicwf_%.o: $(AICWF_DIR)/shim/%.c | $(B)
	$(CC) $(AICWF_CFLAGS) -c $< -o $@

$(B)/aicwf_fw_files.o: $(AICWF_DIR)/shim/aicwf_fw_files.c | $(B)
	$(CC) $(AICWF_CFLAGS) -c $< -o $@

$(B)/aicwf_fw_data.c: | $(B)
	if [ -f "$(AICWF_FW_DATA)" ]; then cp "$(AICWF_FW_DATA)" $@; \
	else echo "aicwf: no firmware at $(AICWF_FW_DATA), building without"; \
	python3 -I $(AICWF_DIR)/gen_fwdata.py --stub --out $@; fi

$(B)/aicwf_fw_data.o: $(B)/aicwf_fw_data.c
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/wifi.o: wifi.c $(AICWF_DIR)/aicwf_wifi.h | $(B)
	$(CC) $(CFLAGS) -I$(AICWF_DIR) -c $< -o $@
