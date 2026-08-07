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

#include "extmod/vfs.h"
#include "extmod/vfs_fat.h"

#include "shared/runtime/gchelper.h"
#include "shared/runtime/pyexec.h"

#include "uart.h"
#include "flash.h"
#include "usbd.h"
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

/* Mount the flash volume at "/", formatting it first if that fails, so a blank
 * board comes up with a working filesystem without user action.
 *
 * mp_vfs_mount_and_chdir_protected catches exceptions itself and returns 0 on
 * success, so only the mkfs call needs its own nlr guard. mkfs is a static
 * method on the VfsFat type rather than a public C function, hence the
 * attribute lookup. */
static void init_filesystem(void) {
    ch32_flashbdev_init();
    mp_obj_t bdev = mp_call_function_0(MP_OBJ_FROM_PTR(&ch32_flash_type));
    mp_obj_t mount_point = MP_OBJ_NEW_QSTR(MP_QSTR__slash_);

    if (mp_vfs_mount_and_chdir_protected(bdev, mount_point) == 0) {
        return;
    }

    mp_printf(&mp_plat_print, "MPY: formatting flash filesystem\n");
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t mkfs = mp_load_attr(MP_OBJ_FROM_PTR(&mp_fat_vfs_type), MP_QSTR_mkfs);
        mp_call_function_1(mkfs, bdev);
        nlr_pop();
    } else {
        mp_printf(&mp_plat_print, "MPY: failed to create filesystem\n");
        return;
    }

    if (mp_vfs_mount_and_chdir_protected(bdev, mount_point) != 0) {
        mp_printf(&mp_plat_print, "MPY: failed to mount filesystem\n");
    }
}

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
    ch32_usbd_init();

    // Leave a margin below the true stack top for the C stack itself.
    mp_stack_set_top((void *)&_eusrstack);
    mp_stack_set_limit(12 * 1024);

    size_t heap_size = (size_t)(&_heap_end - &_heap_start);

    // Outer loop: a soft reset (Ctrl-D) re-initialises the heap and VM rather
    // than resetting the chip, so the console session survives.
    for (;;) {
        gc_init(&_heap_start, &_heap_end);
        mp_init();
        init_filesystem();

        mp_printf(&mp_plat_print, "MicroPython on %s\n", MICROPY_HW_BOARD_NAME);
        mp_printf(&mp_plat_print, "heap: %u bytes\n", (unsigned)heap_size);

        /* Startup scripts, both optional. pyexec_file_if_exists reports any
         * exception to the console and returns, so a bad main.py cannot stop
         * the REPL from coming up and making the board unusable. */
        pyexec_file_if_exists("/boot.py");
        pyexec_file_if_exists("/main.py");

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
