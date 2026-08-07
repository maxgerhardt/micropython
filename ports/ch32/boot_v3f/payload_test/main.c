/* Bare-metal V5F payload, used only to prove the wake mechanism.
 * It is never part of a MicroPython build. */
#include "ch32h417.h"
#include "system_ch32h417.h"

static void uart1_puts(const char *s) {
    while (*s) {
        if (*s == '\n') {
            while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET) {
            }
            USART_SendData(USART1, '\r');
        }
        while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET) {
        }
        USART_SendData(USART1, (uint16_t)(uint8_t)*s++);
    }
}

static void uart1_putu(uint32_t v) {
    char buf[11];
    int i = 0;
    if (v == 0) {
        while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET) {
        }
        USART_SendData(USART1, '0');
        return;
    }
    while (v) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i) {
        while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET) {
        }
        USART_SendData(USART1, (uint16_t)buf[--i]);
    }
}

static void crude_delay(volatile uint32_t n) {
    while (n--) {
        __asm volatile ("nop");
    }
}

int main(void) {
    /* The V3F has already configured every PLL and USART1. Calling
     * SystemInit() here would reconfigure the clock tree underneath a
     * running core -- only refresh the cached clock variables. */
    SystemAndCoreClockUpdate();

    uart1_puts("V5F: alive\n");
    uart1_puts("V5F: core id = ");
    uart1_putu(NVIC_GetCurrentCoreID());
    uart1_puts("\n");
    uart1_puts("V5F: SystemCoreClock = ");
    uart1_putu(SystemCoreClock);
    uart1_puts("\n");

    for (uint32_t i = 0; ; i++) {
        uart1_puts("V5F: tick ");
        uart1_putu(i);
        uart1_puts("\n");
        crude_delay(8000000);
    }
}
