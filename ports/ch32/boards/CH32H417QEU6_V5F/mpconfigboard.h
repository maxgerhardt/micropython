#define MICROPY_HW_BOARD_NAME       "CH32H417QEU6-R0-1v1 (V5F)"
#define MICROPY_HW_MCU_NAME         "CH32H417QEU8 (QingKe V5F)"

/* Console UART: USART1 on PA9 (TX) / PA10 (RX), both AF7. */
#define MICROPY_HW_UART_REPL        (1)
#define MICROPY_HW_UART_REPL_BAUD   (115200)

/* The V3F stub has already configured every PLL before waking this core, so
 * main() must not call SystemInit(). */
#define MICROPY_HW_CORE_IS_V5F      (1)

/* No attach window needed here: this image is not the reset vector, and the
 * V3F stub runs first. */
#define MICROPY_HW_BOOT_DELAY_LOOPS (0)
#define MICROPY_HW_ITCM_HOT_CODE     (1)

/* The GC heap spans two disjoint regions on this board. DTCM holds .data,
 * .bss and the stack as well, which leaves only about 162K of it for the heap;
 * the tail of the shared area past ETH_RAM was unclaimed and adds 144K more.
 * See the .heap2 section in ch32h417_v5f.ld and the gc_add() call in main.c.
 *
 * The two areas are not equivalent. DTCM is zero-wait at the V5F's 400 MHz
 * core clock, while the shared region is reached over the system bus at HCLK:
 * memcpy measures 259 MB/s against 131 MB/s, and a word-wise fill 490 MB/s
 * against 385 MB/s (docs/hw/benchmarks.md). That ordering is why DTCM stays
 * the primary area -- gc_alloc walks the area list from the front and only
 * reaches the second area once the first cannot satisfy a request, so a
 * program that fits in DTCM never pays for the extra region. */
#define MICROPY_GC_SPLIT_HEAP        (1)
