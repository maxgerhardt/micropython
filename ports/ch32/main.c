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

#include "uart.h"
#include "mphalport.h"

// Provided by the linker script.
extern uint8_t _heap_start;
extern uint8_t _heap_end;
extern uint8_t _eusrstack;

static void boot_delay(volatile uint32_t n) {
    while (n--) {
        __asm volatile ("nop");
    }
}

int main(void) {
    /* Attach window before touching clocks or peripherals. If firmware ever
     * wedges the SDI debug interface, this is what lets a debugger back in
     * after a reset. See MICROPY_HW_BOOT_DELAY_LOOPS. */
    #if MICROPY_HW_BOOT_DELAY_LOOPS
    boot_delay(MICROPY_HW_BOOT_DELAY_LOOPS);
    #endif

    SystemInit();
    SystemAndCoreClockUpdate();

    mp_hal_init();
    uart_init(MICROPY_HW_UART_REPL_BAUD);

    // Leave a margin below the true stack top for the C stack itself.
    mp_stack_set_top((void *)&_eusrstack);
    mp_stack_set_limit(12 * 1024);

    size_t heap_size = (size_t)(&_heap_end - &_heap_start);
    gc_init(&_heap_start, &_heap_end);
    mp_init();

    mp_printf(&mp_plat_print, "MicroPython on %s\n", MICROPY_HW_BOARD_NAME);
    mp_printf(&mp_plat_print, "heap: %u bytes\n", (unsigned)heap_size);

    // Stage 1 proof-of-life: run a tiny script through the full compile+exec path.
    const char *src = "print('mp_init ok')";
    mp_lexer_t *lex = mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_, src, strlen(src), 0);
    qstr source_name = lex->source_name;
    mp_parse_tree_t pt = mp_parse(lex, MP_PARSE_FILE_INPUT);
    mp_obj_t module_fun = mp_compile(&pt, source_name, false);
    mp_call_function_0(module_fun);

    mp_deinit();
    for (;;) {
    }
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
