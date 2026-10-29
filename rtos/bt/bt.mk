# Bluetooth (rtos/bt), included by rtos/app/Makefile after aicwf.mk: BTstack (fetched by
# rtos/bt/fetch-btstack.sh, not vendored) + the RTEMS port, linked into nexus-rtos.elf and
# libnexusrtos.a through CONFIG_OBJS. Without BTstack the stub keeps the image linking and the
# `bt` command says Bluetooth is not in the image.
BT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
include $(BT_DIR)/btstack.mk
BTSTACK_DIR ?= $(BT_DIR)/btstack

# main.c includes nexus_bt.h
CFLAGS += -I$(BT_DIR)
$(B)/main.o $(B)/main_n2.o: $(BT_DIR)/nexus_bt.h

ifneq ($(wildcard $(BTSTACK_DIR)/src/hci.c),)
BT_INC := -I$(BT_DIR) $(addprefix -I$(BTSTACK_DIR)/,$(BTSTACK_INC)) -I$(AICWF_DIR) -I$(CURDIR)
# BTstack itself: third-party code, built without warnings noise
BT_STACK_CFLAGS := $(CFLAGS) -std=gnu11 -w $(BT_INC)
BT_PORT_CFLAGS := $(CFLAGS) -std=gnu11 -Wno-unused-parameter $(BT_INC)
BT_OBJS := $(addprefix $(B)/btstack/,$(BTSTACK_SRC:.c=.o)) $(addprefix $(B)/bt_,$(NEXUS_BT_SRC:.c=.o))

$(B)/btstack/%.o: $(BTSTACK_DIR)/%.c $(BT_DIR)/btstack_config.h | $(B)
	@mkdir -p $(dir $@)
	$(CC) $(BT_STACK_CFLAGS) -c $< -o $@

$(B)/bt_%.o: $(BT_DIR)/%.c $(wildcard $(BT_DIR)/*.h) | $(B)
	$(CC) $(BT_PORT_CFLAGS) -c $< -o $@
else
$(info bt: no BTstack at $(BTSTACK_DIR) (run rtos/bt/fetch-btstack.sh); building the Bluetooth stub)
BT_OBJS := $(B)/bt_nexus_bt_stub.o
$(B)/bt_nexus_bt_stub.o: $(BT_DIR)/nexus_bt_stub.c $(BT_DIR)/nexus_bt.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
endif

# the image link line names CONFIG_OBJS (like aicwf.mk)
CONFIG_OBJS += $(BT_OBJS)
$(B)/nexus-rtos.elf: $(BT_OBJS)
