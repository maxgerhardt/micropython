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
    /* Read ra and sp first, before anything in this function can disturb
       them: on a jump to a bad address the return address is usually the only
       record of who jumped, and mepc just says where it landed. */
    uint32_t ra, sp;
    __asm volatile ("mv %0, ra" : "=r" (ra));
    __asm volatile ("mv %0, sp" : "=r" (sp));

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
    fault_puts("\r\nra=");
    fault_puthex(ra);
    fault_puts("\r\nsp=");
    fault_puthex(sp);

    /* Anything on the stack that looks like a code address is a return
     * address, and between them they name the call chain that got here. Both
     * RAM_CODE and flash are checked because this port runs code from each. */
    fault_puts("\r\nstack:");
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t w = ((volatile uint32_t *)sp)[i];
        if ((w >= 0x20000000u && w < 0x20180000u)
            || (w >= 0x08000000u && w < 0x080B0000u)) {
            fault_puts(" ");
            fault_puthex(w);
        }
    }
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
