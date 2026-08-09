# WCH NoneOS SDK sources and includes for the CH32H417 V3F.
#
# Sources are listed explicitly rather than globbed so that pulling in a new
# peripheral for a later milestone (USB, Ethernet, flash filesystem) is a
# visible, reviewable change.
SDK ?= $(subst \,/,$(HOME))/.platformio/packages/framework-wch-noneos-sdk

# Debug/ is on the include path because ch32h417_conf.h pulls in debug.h, but
# debug.c is NOT compiled (see below).
SDK_INC := \
	$(SDK)/Core/ch32h417 \
	$(SDK)/Peripheral/ch32h417/inc \
	$(SDK)/System/ch32h417/$(CH32_CORE) \
	$(SDK)/Debug/ch32h417

INC += $(addprefix -I,$(SDK_INC))

# Note: Debug/ch32h417/debug.c is deliberately absent. It defines a _write
# retarget that collides with MicroPython's stdio.
SDK_SRC_C := \
	$(SDK)/Core/ch32h417/core_riscv.c \
	$(SDK)/System/ch32h417/$(CH32_CORE)/system_ch32h417.c \
	$(SDK)/Peripheral/ch32h417/src/ch32h417_rcc.c \
	$(SDK)/Peripheral/ch32h417/src/ch32h417_gpio.c \
	$(SDK)/Peripheral/ch32h417/src/ch32h417_i2c.c \
	$(SDK)/Peripheral/ch32h417/src/ch32h417_spi.c \
	$(SDK)/Peripheral/ch32h417/src/ch32h417_adc.c \
	$(SDK)/Peripheral/ch32h417/src/ch32h417_tim.c \
	$(SDK)/Peripheral/ch32h417/src/ch32h417_dac.c \
	$(SDK)/Peripheral/ch32h417/src/ch32h417_iwdg.c \
	$(SDK)/Peripheral/ch32h417/src/ch32h417_usart.c \
	$(SDK)/Peripheral/ch32h417/src/ch32h417_flash.c \
	$(SDK)/Peripheral/ch32h417/src/ch32h417_pwr.c \
	$(SDK)/Peripheral/ch32h417/src/ch32h417_misc.c

# Drop any that do not exist in this SDK revision.
SDK_SRC_C := $(wildcard $(SDK_SRC_C))

# Startup: use the port's copy when there is one, otherwise the SDK's. The V5F
# file is vendored here because it has to enter main() in Machine mode rather
# than the User mode WCH's stock version selects -- see the comment on mstatus
# inside it.
SDK_SRC_S := $(if $(wildcard $(CH32_STARTUP)),$(CH32_STARTUP),$(SDK)/Startup/$(CH32_STARTUP))
