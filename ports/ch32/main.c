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

#if MICROPY_PY_NETWORK
#include "extmod/modnetwork.h"
#endif
#if MICROPY_PY_LWIP
#include "lwip/apps/mdns.h"
#include "lwip/init.h"
#include "lwip/timeouts.h"
#endif

#include "shared/runtime/gchelper.h"
#include "shared/runtime/pyexec.h"

#include "uart.h"
#include "flash.h"
#include "usbd.h"
#include "machine_pin.h"
#include "machine_dac.h"

/* Defined in machine_uart.c, which extmod/machine_uart.c includes rather than
 * compiling on its own, so there is no header to take this from. */
#if MICROPY_PY_MACHINE_UART
void machine_uart_deinit_all(void);
#endif
#if MICROPY_PY_MACHINE_CAN
#include "extmod/machine_can.h"
#endif
#include "machine_pwm.h"
#include "machine_mem_backup.h"
#include "machine_counter.h"
#include "machine_encoder.h"
#include "machine_rtc.h"
#include "machine_vio18.h"

/* Defined in machine_sdcard.c; nothing else in the port needs its type. */
void machine_sdcard_deinit_all(void);
void machine_audioout_deinit_all(void);
#if MICROPY_PY_MACHINE_I2S
void machine_i2s_deinit_all(void);
#endif
#include "machine_timer.h"
#include "machine_wdt.h"
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
#if MICROPY_GC_SPLIT_HEAP
extern uint8_t _sheap2;
extern uint8_t _eheap2;
#endif

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

    /* Read the RCC reset flags before anything else can clear them. */
    machine_wdt_reset_cause_init();

    mp_hal_init();
    /* After the timebase, because the rail needs a settling delay, and before
     * any pin is configured: most of this chip's pads are on VIO18, whose LDO
     * powers up at 1.2 V, and this raises it to 3.3 V so that every pin
     * drives what a MicroPython program expects it to. The REPL UART is on
     * VDDIO and does not care either way. */
    ch32_vio18_init();
    uart_init(MICROPY_HW_UART_REPL_BAUD);
    /* After the timebase and the console, not before: starting the RTC waits on
     * an oscillator with a timeout, and a timeout needs a clock to measure. Run
     * ahead of mp_hal_init() it spun forever with no output at all. */
    machine_rtc_init_boot();
    /* Outside the soft-reset loop below: this decides whether the backup
     * region survived, and re-deciding that on every soft reset would
     * be re-deciding it wrongly. */
    ch32_mem_backup_init();
    ch32_usbd_init();

    #if MICROPY_PY_LWIP
    /* Once, outside the soft-reset loop. lwip cannot be reinitialised by
     * calling this again: its timeout list is static and only a real reset
     * clears it, so a second call leaks every pending timer.
     *
     * It must happen before any netif exists. Without it the memp pools are
     * never built, pbuf_alloc() returns NULL for every frame, and the
     * interface comes up, negotiates, reports link -- and silently drops
     * 100% of traffic. */
    lwip_init();
    #if LWIP_MDNS_RESPONDER
    mdns_resp_init();
    #endif
    #endif

    /* Fill the unused stack with a known pattern so ch32.stack_usage() can
     * find the high-water mark later. Painting stops well below the current
     * stack pointer, so it cannot scribble on the frame doing the painting.
     *
     * This is not diagnostics for its own sake: the stack is immediately
     * below nothing -- it grows down towards the heap -- so an overflow
     * corrupts the GC heap silently instead of trapping. Being able to ask how
     * close it came is the only cheap defence. */
    ch32_stack_paint();

    /* Leave a margin below the true stack top for the C stack itself. The
     * limit is what the VM checks against; the gap between it and the real
     * 32K is what C call chains MicroPython cannot see -- lwip's receive path
     * above all -- get to use. */
    mp_stack_set_top((void *)&_eusrstack);
    mp_stack_set_limit(24 * 1024);

    size_t heap_size = (size_t)(&_heap_end - &_heap_start);
    #if MICROPY_GC_SPLIT_HEAP
    heap_size += (size_t)(&_eheap2 - &_sheap2);
    #endif

    // Outer loop: a soft reset (Ctrl-D) re-initialises the heap and VM rather
    // than resetting the chip, so the console session survives.
    for (;;) {
        gc_init(&_heap_start, &_heap_end);
        #if MICROPY_GC_SPLIT_HEAP
        /* Inside the loop, not before it: gc_init() rebuilds the area list
         * from scratch and drops every area that was added, so a soft reset
         * would otherwise silently lose the second region and come back with
         * a heap less than half the size it had. */
        gc_add(&_sheap2, &_eheap2);
        #endif
        mp_init();

        /* mp_init() leaves sys.path as ['', '.frozen'], which does not include
         * /lib -- so `mpremote mip install` wrote packages to a directory
         * nothing would ever import from, and the installed module was simply
         * invisible. Every other port appends this; see ports/rp2/main.c. */
        mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR__slash_lib));

        init_filesystem();

        /* Inside the loop, unlike lwip_init(): this rebuilds the NIC list,
         * which lives in a root pointer that mp_init() has just cleared. */
        #if MICROPY_PY_NETWORK
        mod_network_init();
        #endif

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

        /* Silence pin interrupts before the heap holding their handlers is
         * reclaimed, so a stray edge cannot dispatch into freed memory. */
        machine_pin_deinit();

        /* Same reasoning for the RTC alarm: its handler is a heap object, and
         * the alarm keeps running in the backup domain across a soft reset, so
         * one left armed would dispatch into reclaimed memory when it fired. */
        machine_rtc_irq_deinit();

        /* Stop the PWM outputs too. The timers keep running on their own once
         * started, so without this a soft reset would leave a servo or a motor
         * driver at whatever duty the program that just exited had set, with
         * every PWM object that knew about it already collected. */
        #if MICROPY_PY_MACHINE_PWM
        machine_pwm_deinit_all();
        #endif

        /* And the hardware timers, for the same reason as the pins: the
         * counter keeps running and its callback is a heap object. */
        machine_timer_deinit_all();
        machine_counter_deinit_all();
        machine_encoder_deinit_all();

        /* And the card, whose object is about to be freed while the
         * controller would otherwise still be clocking a selected card. */
        machine_sdcard_deinit_all();

        /* And the audio output, which is a timer, a DMA channel and two
         * analog pins that would otherwise keep converting a freed buffer. */
        machine_audioout_deinit_all();

        #if MICROPY_PY_MACHINE_I2S
        /* And I2S, whose DMA buffer lives inside the object about to be
         * freed. Leaving it running corrupts the next program's heap. */
        machine_i2s_deinit_all();
        #endif

        /* Same for the analog outputs, which otherwise hold their last voltage
         * indefinitely. */
        machine_dac_deinit_all();

        /* And the UARTs. Their objects live on the heap that is about to be
         * reclaimed, but the table the receive interrupt finds them through is
         * an ordinary static and survives, so without this a byte arriving
         * after a soft reset would be pushed into a freed ring buffer -- and
         * the next UART(id) would read its settings out of the same freed
         * object. The upstream machine_uart_tx test caught exactly that, as a
         * construction failing on parameters it had never been given. */
        #if MICROPY_PY_MACHINE_UART
        machine_uart_deinit_all();
        #endif

        /* And the CAN controllers, for the same reason with an extra edge: a
         * bxCAN left running keeps acknowledging frames on a live bus, so
         * every other node goes on believing this one is there while its
         * driver state has been reclaimed. */
        #if MICROPY_PY_MACHINE_CAN
        machine_can_deinit_all();
        #endif

        /* The netif deliberately stays up across a soft reset, so an Ethernet
         * link and its DHCP lease survive Ctrl-D rather than having to be
         * renegotiated. That is safe only because every buffer the receive
         * interrupt touches -- descriptors, MAC buffers, lwip's pools -- is
         * static, so none of it is on the heap being reclaimed here. */
        #if MICROPY_PY_NETWORK
        mod_network_deinit();
        #endif

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
