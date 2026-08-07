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
