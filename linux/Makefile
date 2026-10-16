# Linux host build for the GB/T 27930 BMS logic.
#
# The protocol state machine and the runtime CLI are the same source files the
# STM32 firmware uses (../Core/Src).  Hardware halves are replaced here:
#   CAN  -> SocketCAN (can_socketcan.c) or socketcand TCP tunnel (can_socketcand.c)
#   CLI  -> MQTT         (mqtt_cli.c)
#
# Requirements: gcc, make, libmosquitto (Arch: pacman -S mosquitto)

CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra
CFLAGS  += -std=gnu11 -I../Core/Inc -I.
LDLIBS  += -lmosquitto

BUILD   := build
TARGET  := $(BUILD)/gbt-linux

CORE_DIR := ../Core/Src

SRCS := \
	$(CORE_DIR)/gbt_27930_bms.c \
	$(CORE_DIR)/can_buffer.c \
	$(CORE_DIR)/gbt_cli.c \
	$(CORE_DIR)/settings_store.c \
	can_socketcan.c \
	can_socketcand.c \
	can_dispatch.c \
	helpers_linux.c \
	settings_nvm_file.c \
	mqtt_cli.c \
	main.c

OBJS := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(SRCS)))

vpath %.c $(CORE_DIR) .

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)

.PHONY: all clean
