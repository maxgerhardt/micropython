# Shared between the V3F stub build and the V5F MicroPython build.
#
# NVIC_WakeUp_V5F() masks the address with ~0x3FF, so this MUST stay 1 KB
# aligned. It is also the ORIGIN of the FLASH region in the V5F linker
# scripts; those carry the literal value with a comment pointing here.
CH32_V5F_FLASH_ORIGIN = 0x00010000
