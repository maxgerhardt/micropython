# Bare-metal V5F payload, used only to prove the wake mechanism.
# MicroPython's build system: this image contains no MicroPython at all.
SHELL := /usr/bin/env bash

include ../../coreboot.mk

PIO_PKGS ?= $(subst \,/,$(HOME))/.platformio/packages
CROSS    ?= $(PIO_PKGS)/toolchain-riscv/bin/riscv-wch-elf-
SDK      ?= $(PIO_PKGS)/framework-wch-noneos-sdk

CC   := $(CROSS)gcc
SIZE := $(CROSS)size

BUILD  := build
TARGET := $(BUILD)/payload.elf

ARCHFLAGS := -march=rv32imafc_zba_zbb_zbc_zbs_xw -mabi=ilp32f

INC := -I$(SDK)/Core/ch32h417 -I$(SDK)/Peripheral/ch32h417/inc \
       -I$(SDK)/System/ch32h417/v5f -I$(SDK)/Debug/ch32h417 -I.

CFLAGS := $(ARCHFLAGS) -Os -g -std=gnu11 -fsingle-precision-constant \
	-msmall-data-limit=8 -mno-save-restore \
	-ffunction-sections -fdata-sections -Wall -Wno-comment \
	-DCH32H417QE -DCH32H41X -DCH32H41x -DCH32H417 -DCore_V5F \
	-DSYSCLK_400M_CoreCLK_V5F_400M_V3F_100M_HSI \
	$(INC)

LDFLAGS := $(ARCHFLAGS) -nostartfiles -Wl,--gc-sections \
	-Wl,-Map=$(BUILD)/payload.map -T ch32h417_v5f_test.ld

SRC_C := main.c \
	$(SDK)/Core/ch32h417/core_riscv.c \
	$(SDK)/System/ch32h417/v5f/system_ch32h417.c \
	$(SDK)/Peripheral/ch32h417/src/ch32h417_rcc.c \
	$(SDK)/Peripheral/ch32h417/src/ch32h417_gpio.c \
	$(SDK)/Peripheral/ch32h417/src/ch32h417_usart.c \
	$(SDK)/Peripheral/ch32h417/src/ch32h417_flash.c \
	$(SDK)/Peripheral/ch32h417/src/ch32h417_pwr.c

SRC_S := $(SDK)/Startup/startup_ch32h417_v5f.S

OBJ := $(addprefix $(BUILD)/,$(notdir $(SRC_C:.c=.o))) \
       $(addprefix $(BUILD)/,$(notdir $(SRC_S:.S=.o)))

VPATH := . $(sort $(dir $(SRC_C) $(SRC_S)))

all: $(TARGET)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJ)
	$(CC) $(OBJ) $(LDFLAGS) -o $@
	$(SIZE) $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
