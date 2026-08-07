#define MICROPY_HW_BOARD_NAME       "CH32H417QEU6-R0-1v1"
#define MICROPY_HW_MCU_NAME         "CH32H417QEU8 (QingKe V3F)"

/* Console UART: USART1 on PA9 (TX) / PA10 (RX), both AF7. */
#define MICROPY_HW_UART_REPL        (1)
#define MICROPY_HW_UART_REPL_BAUD   (115200)

/* Delay at the very top of main() before any clock or peripheral setup.
 * If firmware ever wedges the SDI debug interface, this window is what lets
 * a debugger attach after a reset. Keep non-zero during bring-up. */
#define MICROPY_HW_BOOT_DELAY_LOOPS (20000000)
