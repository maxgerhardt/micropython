CH32_CORE = v3f
CH32_STARTUP = startup_ch32h417_v3f.S
LD_FILE = boards/CH32H417QEU6_V3F/ch32h417_v3f.ld

# Chip selection macros, matching the PlatformIO board definition.
CFLAGS_BOARD += -DCH32H417QE -DCH32H41X -DCH32H41x -DCH32H417 -DCore_V3F
# V3F lands on exactly 100 MHz from the internal 25 MHz HSI (no crystal needed).
# The value matters: system_ch32h417_v3f.c initialises SystemClock from this
# macro, so defining it bare made SystemClock 1 until SystemAndCoreClockUpdate()
# recomputed it. 400000000 is what the vendor's own commented-out #define says.
CFLAGS_BOARD += -DSYSCLK_400M_CoreCLK_V5F_400M_V3F_100M_HSE=400000000

MICROPY_FLOAT_IMPL = float
SUPPORTS_HARDWARE_FP_SINGLE = 1
