# MicroPython port for the WCH CH32H417 (QingKe V3F core).
BOARD ?= CH32H417QEU6_V3F
BOARD_DIR ?= boards/$(BOARD)

ifeq ($(wildcard $(BOARD_DIR)/.),)
$(error Invalid BOARD specified: $(BOARD_DIR))
endif

BUILD ?= build-$(BOARD)

include ../../py/mkenv.mk
-include mpconfigport.mk
include $(BOARD_DIR)/mpconfigboard.mk

# qstr definitions (must come before including py.mk)
QSTR_DEFS = qstrdefsport.h

MICROPY_ROM_TEXT_COMPRESSION ?= 1
MICROPY_FLOAT_IMPL ?= float

# Must be set before extmod.mk, which adds lib/oofatfs to SRC_THIRDPARTY_C when
# it sees this. Adding those sources to SRC_C by hand instead would drag them
# into SRC_QSTR, and the qstr preprocessor pass chokes on ff.c's token pasting.
MICROPY_VFS_FAT = 1

include $(TOP)/py/py.mk
include $(TOP)/extmod/extmod.mk

# Toolchain: WCH's GCC, the only one that understands the custom "xw" extension.
#
# $(HOME) is a Windows-style path under Git Bash (C:\Users\...), and the
# backslashes get swallowed when recipes are re-quoted through the shell.
# Normalise to forward slashes, which both sh and the native gcc accept.
HOME_FWD := $(subst \,/,$(HOME))
PIO_PKGS ?= $(HOME_FWD)/.platformio/packages
CROSS_COMPILE ?= $(PIO_PKGS)/toolchain-riscv/bin/riscv-wch-elf-
SDK ?= $(PIO_PKGS)/framework-wch-noneos-sdk

include sdk.mk

ARCHFLAGS = -march=rv32imafc_zba_zbb_zbc_zbs_xw -mabi=ilp32f

INC += -I. -I$(TOP) -I$(BUILD) -I$(BOARD_DIR)

ifeq ($(DEBUG),1)
COPT ?= -Og -g3
else
COPT ?= -Os -g -DNDEBUG
endif

CFLAGS += $(INC) $(ARCHFLAGS) $(CFLAGS_BOARD) \
	-std=gnu11 -Wall -Wno-comment \
	-msmall-data-limit=8 -mno-save-restore \
	-ffunction-sections -fdata-sections \
	-fsingle-precision-constant \
	-DMICROPY_FLOAT_IMPL=MICROPY_FLOAT_IMPL_FLOAT \
	-DMICROPY_HW_FPU=1 \
	$(COPT) $(CFLAGS_EXTRA)

# mkrules.mk assembles .s files with "$(AS) $(AFLAGS)", which does not inherit
# CFLAGS. Without the arch flags here the assembler emits a default-ABI object
# and the link fails with "can't link double-float modules with single-float".
AFLAGS += $(ARCHFLAGS)

LDFLAGS += $(ARCHFLAGS) -nostartfiles -Wl,--gc-sections \
	-Wl,-Map=$@.map -T $(LD_FILE)

LIBS += -lm

SRC_C += \
	main.c \
	uart.c \
	flash.c \
	flashbdev.c \
	modch32.c \
	machine_pin.c \
	usbd.c \
	msc_disk.c \
	mphalport.c \
	fault.c \
	shared/timeutils/timeutils.c \
	shared/readline/readline.c \
	shared/runtime/gchelper_native.c \
	shared/runtime/interrupt_char.c \
	shared/runtime/pyexec.c \
	shared/runtime/stdout_helpers.c \
	shared/runtime/sys_stdio_mphal.c \

# RISC-V register-spilling helper, so the GC scans callee-saved registers.
SRC_S += shared/runtime/gchelper_rv32i.s

# Scan our sources for qstrs and MP_REGISTER_ROOT_POINTER declarations.
# Without this, e.g. readline's history root pointer never reaches
# genhdr/root_pointers.h and MP_STATE_PORT(readline_hist) fails to compile.
SRC_QSTR += $(SRC_C)

# extmod.mk puts third-party sources (oofatfs) here; they are deliberately not
# in SRC_C so that SRC_QSTR does not scan them.
# USB device stack. TinyUSB sources go in SRC_THIRDPARTY_C so that SRC_QSTR
# does not scan them, exactly as oofatfs does.
TINYUSB_DIR = lib/tinyusb/src
INC += -I$(TOP)/$(TINYUSB_DIR) -I$(TOP)/shared/tinyusb
CFLAGS += -DCFG_TUSB_MCU=OPT_MCU_CH32H417 -DCFG_TUSB_OS=OPT_OS_NONE

# TinyUSB ships its own source list; use it rather than naming files here, so that
# upstream moving a file around (usbd_control.c folded into usbd.c, say) does not
# break this Makefile. Everything not enabled in mpconfigport.h is #if'd out to an
# empty translation unit, so the classes we do not use cost nothing but a compile.
# This follows ports/samd and ports/mimxrt.
TINYUSB_SRC_C :=
-include $(TOP)/$(TINYUSB_DIR)/tinyusb.mk

SRC_THIRDPARTY_C += $(addprefix lib/tinyusb/, \
	$(TINYUSB_SRC_C) \
	src/portable/wch/dcd_ch32_usbfs.c \
	)

SRC_C += \
	shared/tinyusb/mp_usbd.c \
	shared/tinyusb/mp_usbd_cdc.c \
	shared/tinyusb/mp_usbd_descriptor.c \

CFLAGS += $(CFLAGS_EXTMOD) $(CFLAGS_THIRDPARTY)

OBJ += $(PY_O)
OBJ += $(addprefix $(BUILD)/, $(SRC_C:.c=.o))
OBJ += $(addprefix $(BUILD)/, $(SRC_THIRDPARTY_C:.c=.o))
OBJ += $(addprefix $(BUILD)/, $(SRC_S:.s=.o))
OBJ += $(addprefix $(BUILD)/sdk/, $(notdir $(SDK_SRC_C:.c=.o)))
OBJ += $(addprefix $(BUILD)/sdk/, $(notdir $(SDK_SRC_S:.S=.o)))
# Deduplicate: a source reaching OBJ twice becomes a multiple-definition link error.
OBJ := $(sort $(OBJ))

# The vendor SDK lives outside TOP, so give make somewhere to find it.
vpath %.c $(sort $(dir $(SDK_SRC_C)))
vpath %.S $(sort $(dir $(SDK_SRC_S)))

# SDK objects go in their own subdirectory. This pattern has a shorter stem
# than mkrules.mk's generic $(BUILD)/%.o, so make prefers it.
$(BUILD)/sdk/%.o: %.c
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "CC $<"
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/sdk/%.o: %.S
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "AS $<"
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

all: $(BUILD)/firmware.elf

$(BUILD)/firmware.elf: $(OBJ) $(LD_FILE)
	$(ECHO) "LINK $@"
	$(Q)$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LIBS)
	$(Q)$(SIZE) $@

include coreboot.mk

BOOT_V3F_ELF = boot_v3f/build/boot_v3f.elf
SCRIPTS = ../../../scripts

$(BOOT_V3F_ELF):
	$(MAKE) -C boot_v3f

ifeq ($(CH32_CORE),v5f)
# The flash driver mass-erases on every program command, so the stub and the
# MicroPython image cannot be written as two operations -- the second wipes the
# first while still reporting "Verified OK". They are merged and written once.
deploy: $(BUILD)/firmware.bin
	bash $(SCRIPTS)/flash.sh $(BUILD)/firmware.bin 0

$(BUILD)/firmware.bin: $(BUILD)/firmware.elf $(BOOT_V3F_ELF)
	$(ECHO) "MERGE $@"
	$(Q)$(PYTHON) $(SCRIPTS)/merge_flash.py -o $@ $(BOOT_V3F_ELF) $(BUILD)/firmware.elf

firmware.bin: $(BUILD)/firmware.bin
else
deploy: $(BUILD)/firmware.elf
	bash $(SCRIPTS)/flash.sh $(BUILD)/firmware.elf 0

firmware.bin: $(BUILD)/firmware.elf
	$(Q)$(OBJCOPY) -O binary $< $(BUILD)/firmware.bin
endif

.PHONY: deploy firmware.bin

include $(TOP)/py/mkrules.mk
