# Standalone build of the V3F boot stub. Deliberately independent of
# MicroPython's build system: this image contains no MicroPython at all.
SHELL := /usr/bin/env bash

include ../coreboot.mk

# The toolchain still defaults to the PlatformIO package store, which is where
# it lives on a development machine; CI overrides CROSS. The SDK does not: it is
# the vendored submodule, so this build is reproducible from a git checkout.
PIO_PKGS ?= $(subst \,/,$(HOME))/.platformio/packages
CROSS    ?= $(PIO_PKGS)/toolchain-riscv/bin/riscv-wch-elf-
SDK      ?= ../../../lib/ch32h417lib

CC   := $(CROSS)gcc
SIZE := $(CROSS)size

BUILD  := build
TARGET := $(BUILD)/boot_v3f.elf

ARCHFLAGS := -march=rv32imafc_zba_zbb_zbc_zbs_xw -mabi=ilp32f

# -I.. picks up the port's ch32h417_conf.h and system_ch32h417.h, which upstream
# ships per-example rather than in EVT/EXAM/SRC -- see ../sdk.mk.
INC := -I$(SDK)/Core -I$(SDK)/Peripheral/inc -I$(SDK)/Debug -I.. -I.

CFLAGS := $(ARCHFLAGS) -Os -g -std=gnu11 -fsingle-precision-constant \
	-msmall-data-limit=8 -mno-save-restore \
	-ffunction-sections -fdata-sections -Wall -Wno-comment \
	-DCH32H417QE -DCH32H41X -DCH32H41x -DCH32H417 -DCore_V3F \
	-DSYSCLK_400M_CoreCLK_V5F_400M_V3F_100M_HSE=400000000 \
	-DCH32_V5F_START_ADDR=$(CH32_V5F_FLASH_ORIGIN) \
	$(INC)

LDFLAGS := $(ARCHFLAGS) -nostartfiles -Wl,--gc-sections \
	-Wl,-Map=$(BUILD)/boot_v3f.map -T ch32h417_v3f_boot.ld

SRC_C := main.c \
	$(SDK)/Core/core_riscv.c \
	../system_ch32h417_v3f.c \
	$(SDK)/Peripheral/src/ch32h417_rcc.c \
	$(SDK)/Peripheral/src/ch32h417_gpio.c \
	$(SDK)/Peripheral/src/ch32h417_usart.c \
	$(SDK)/Peripheral/src/ch32h417_flash.c \
	$(SDK)/Peripheral/src/ch32h417_pwr.c \
	$(SDK)/Peripheral/src/ch32h417_exti.c

SRC_S := $(SDK)/Startup/startup_ch32h417_v3f.S

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
