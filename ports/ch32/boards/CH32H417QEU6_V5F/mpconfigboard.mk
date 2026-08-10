CH32_CORE = v5f
CH32_STARTUP = startup_ch32h417_v5f.S
LD_FILE = boards/CH32H417QEU6_V5F/ch32h417_v5f.ld

CFLAGS_BOARD += -DCH32H417QE -DCH32H41X -DCH32H41x -DCH32H417 -DCore_V5F
# The value matters: system_ch32h417_v3f.c initialises SystemClock from this
# macro, so defining it bare made SystemClock 1 until SystemAndCoreClockUpdate()
# recomputed it. 400000000 is what the vendor's own commented-out #define says.
CFLAGS_BOARD += -DSYSCLK_400M_CoreCLK_V5F_400M_V3F_100M_HSE=400000000

MICROPY_FLOAT_IMPL = float
SUPPORTS_HARDWARE_FP_SINGLE = 1
