#include <stdint.h>
#include <string.h>

#include "ch32h417.h"
#include "system_ch32h417.h"

#include "py/compile.h"
#include "py/runtime.h"
#include "py/gc.h"
#include "py/stackctrl.h"
#include "py/mperrno.h"
#include "py/mphal.h"

#include "shared/runtime/gchelper.h"
#include "shared/runtime/pyexec.h"

#include "uart.h"
#include "mphalport.h"

#ifndef MICROPY_HW_ITCM_HOT_CODE
#define MICROPY_HW_ITCM_HOT_CODE (0)
#endif

#ifndef MICROPY_HW_CORE_IS_V5F
#define MICROPY_HW_CORE_IS_V5F (0)
#endif

// Provided by the linker script.
extern uint8_t _heap_start;
extern uint8_t _heap_end;
extern uint8_t _eusrstack;

#if MICROPY_HW_BOOT_DELAY_LOOPS
static void boot_delay(volatile uint32_t n) {
    while (n--) {
        __asm volatile ("nop");
    }
}
#endif

int main(void) {
    /* Attach window before touching clocks or peripherals. If firmware ever
     * wedges the SDI debug interface, this is what lets a debugger back in
     * after a reset. See MICROPY_HW_BOOT_DELAY_LOOPS. */
    #if MICROPY_HW_BOOT_DELAY_LOOPS
    boot_delay(MICROPY_HW_BOOT_DELAY_LOOPS);
    #endif

    #if MICROPY_HW_ITCM_HOT_CODE
    /* Copy the hot section into ITCM, the only memory that is zero-wait at the
     * V5F's 400 MHz core clock. The SDK startup file copies .highcode but knows
     * nothing about this section, so it is done here -- before any of the code
     * it contains is called. */
    {
        extern uint32_t _itcm_lma, _itcm_vma_start, _itcm_vma_end;
        uint32_t *src = &_itcm_lma;
        uint32_t *dst = &_itcm_vma_start;
        while (dst < &_itcm_vma_end) {
            *dst++ = *src++;
        }
    }
    #endif

    #if MICROPY_HW_CORE_IS_V5F
    /* The V3F stub configured every PLL before waking this core. Re-running
     * SystemInit() here would reconfigure the clock tree underneath a running
     * core; only refresh the cached clock variables. */
    #else
    SystemInit();
    #endif
    SystemAndCoreClockUpdate();

    mp_hal_init();
    uart_init(MICROPY_HW_UART_REPL_BAUD);

    // Leave a margin below the true stack top for the C stack itself.
    mp_stack_set_top((void *)&_eusrstack);
    mp_stack_set_limit(12 * 1024);

    size_t heap_size = (size_t)(&_heap_end - &_heap_start);

    // Outer loop: a soft reset (Ctrl-D) re-initialises the heap and VM rather
    // than resetting the chip, so the console session survives.
    for (;;) {
        gc_init(&_heap_start, &_heap_end);
        mp_init();

        mp_printf(&mp_plat_print, "MicroPython on %s\n", MICROPY_HW_BOARD_NAME);
        mp_printf(&mp_plat_print, "heap: %u bytes\n", (unsigned)heap_size);

        // Ctrl-A switches to the raw REPL, which is how tooling (run-tests.py,
        // mpremote) drives the board. Without dispatching on the mode here,
        // the friendly REPL is all that is ever offered.
        for (;;) {
            if (pyexec_mode_kind == PYEXEC_MODE_RAW_REPL) {
                if (pyexec_raw_repl() != 0) {
                    break;   // Ctrl-D in raw REPL requests a soft reset
                }
            } else {
                if (pyexec_friendly_repl() != 0) {
                    break;   // Ctrl-D requests a soft reset
                }
            }
        }

        mp_printf(&mp_plat_print, "MPY: soft reboot\n");
        mp_deinit();
    }
}

// There is no filesystem until the littlefs milestone, but pyexec references
// this for its run-a-file path. Fail cleanly rather than failing to link.
mp_lexer_t *mp_lexer_new_from_file(qstr filename) {
    (void)filename;
    mp_raise_OSError(MP_ENOENT);
}

// The GC must see roots held only in callee-saved registers, so spill them via
// the RISC-V helper (shared/runtime/gchelper_rv32i.s) before scanning the stack.
void gc_collect(void) {
    gc_collect_start();
    gc_helper_collect_regs_and_stack();
    gc_collect_end();
}

// Called by py/runtime.c when an NLR jump has nowhere to go.
void nlr_jump_fail(void *val) {
    (void)val;
    mp_hal_stdout_tx_strn("FATAL: nlr_jump_fail\r\n", 22);
    for (;;) {
    }
}

void MP_WEAK __assert_func(const char *file, int line, const char *func, const char *expr) {
    mp_printf(&mp_plat_print, "assert %s:%d %s %s\n", file, line, func, expr);
    for (;;) {
    }
}
