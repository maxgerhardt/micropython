/* Trap diagnostics. Overrides the weak handlers in startup_ch32h417_v3f.S. */
#include "ch32h417.h"
#include "uart.h"
#include "irq.h"

static void fault_puts(const char *s) {
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    uart_tx_strn(s, n);
}

static void fault_puthex(uint32_t v) {
    static const char hex[] = "0123456789abcdef";
    char buf[11] = "0x00000000";
    for (int i = 0; i < 8; i++) {
        buf[9 - i] = hex[(v >> (i * 4)) & 0xf];
    }
    uart_tx_strn(buf, 10);
}

static void fault_report(const char *kind) {
    uint32_t mcause, mepc, mtval, mstatus;
    __asm volatile ("csrr %0, mcause"  : "=r" (mcause));
    __asm volatile ("csrr %0, mepc"    : "=r" (mepc));
    __asm volatile ("csrr %0, mtval"   : "=r" (mtval));
    __asm volatile ("csrr %0, mstatus" : "=r" (mstatus));

    fault_puts("\r\n=== ");
    fault_puts(kind);
    fault_puts(" ===\r\nmcause=");
    fault_puthex(mcause);
    fault_puts("\r\nmepc=");
    fault_puthex(mepc);
    fault_puts("\r\nmtval=");
    fault_puthex(mtval);
    fault_puts("\r\nmstatus=");
    fault_puthex(mstatus);
    fault_puts("\r\nresetting...\r\n");

    for (volatile uint32_t i = 0; i < 2000000; i++) {
    }
    NVIC_SystemReset();
}

void CH32_IRQ_HANDLER(HardFault_Handler);
void HardFault_Handler(void) {
    fault_report("HARDFAULT");
}

void CH32_IRQ_HANDLER(NMI_Handler);
void NMI_Handler(void) {
    fault_report("NMI");
}
