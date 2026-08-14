/* machine module for the CH32H417.
 * Included by extmod/modmachine.c via MICROPY_PY_MACHINE_INCLUDEFILE, so the
 * mp_machine_* hooks below must keep the static linkage declared there. */
#include "ch32h417.h"
#include "py/runtime.h"
#include "py/mphal.h"

/* machine.Pin lives in machine_pin.c, which is a normal compilation unit
 * rather than part of this include-file, because it is large and other port
 * code needs the mp_hal_pin_* API without pulling in modmachine. */
#include "machine_pin.h"

/* machine.DAC is a whole class defined in its own file, so it needs its type
 * object here to be reachable from the module globals below. */
#include "machine_dac.h"
#include "machine_rtc.h"
#include "machine_timer.h"
#include "machine_sleep.h"
#include "machine_wdt.h"

/* --- module-level hooks required by extmod/modmachine.c --- */

static void mp_machine_idle(void) {
    __asm volatile ("wfi");
}

static mp_obj_t mp_machine_unique_id(void) {
    /* 64 bits, not the 96 this used to return.
     *
     * The ESIG area really does hold eight programmed bytes and then erased
     * flash. Dumped on this part:
     *
     *   1ffff7e8  5eb50116
     *   1ffff7ec  6c080c3d
     *   1ffff7f0  e339e339   <- CH32_FLASH_ERASED_WORD
     *   1ffff7f4  e339e339
     *
     * so the previous twelve bytes ended ...39e339e3, which is not an
     * identifier at all and would have collided across every CH32H417 for any
     * caller that looked only at the tail. wlink agrees, reporting
     * UID(16-01-b5-5e-3d-0c-08-6c).
     *
     * These are the same fuses the Ethernet MAC comes from -- eth.c reads the
     * first six bytes backwards, which is the order WCHNET_GetMacAddr() uses
     * and the order printed on the label. So unique_id() is not the MAC, but
     * its first six bytes reversed are. */
    const uint8_t *id = (const uint8_t *)0x1FFFF7E8;
    return mp_obj_new_bytes(id, 8);
}

static mp_obj_t mp_machine_get_freq(void) {
    return MP_OBJ_NEW_SMALL_INT(SystemCoreClock);
}

static void mp_machine_set_freq(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    (void)args;
    mp_raise_NotImplementedError(MP_ERROR_TEXT("cannot change frequency"));
}

static void mp_machine_lightsleep(size_t n_args, const mp_obj_t *args) {
    machine_sleep_light(n_args != 0 ? mp_obj_get_int(args[0]) : -1);
}

MP_NORETURN static void mp_machine_deepsleep(size_t n_args, const mp_obj_t *args) {
    machine_sleep_deep(n_args != 0 ? mp_obj_get_int(args[0]) : -1);
}

MP_NORETURN static void mp_machine_reset(void) {
    NVIC_SystemReset();
    for (;;) {
    }
}

/* Latched out of the RCC flags at startup, because they accumulate across
 * resets and would otherwise report every reason since power-on. */
static mp_int_t mp_machine_reset_cause(void) {
    return machine_wdt_reset_cause();
}

/* micropython-lib's dht.py looks for machine.dht_readinto first, so exposing
 * it here is what makes the stock DHT11/DHT22 driver work unmodified. */
#include "drivers/dht/dht.h"

/* The sleep modes, as a bitmask, for the wake= argument of Pin.irq() and
 * RTC.irq(). The numbering is the one every other port uses.
 *
 * Both irq() implementations here accept wake and ignore it, and that is
 * honest rather than lazy: a pin interrupt is an EXTI line and the RTC alarm
 * is EXTI line 17, and EXTI is not clock-gated in Stop, so anything that can
 * interrupt at all can also wake the chip. There is nothing to select.
 *
 * machine.wake_reason() is deliberately absent for the same reason there is
 * nothing to select. lightsleep(ms) is a delay loop over WFI, so SysTick wakes
 * the core every millisecond and no single event is "the" wake; deepsleep()
 * resets on the way out, so what woke it is reset_cause() == DEEPSLEEP_RESET
 * and there is no second thing to report. */
enum {
    CH32_WAKE_IDLE = 0x01,
    CH32_WAKE_SLEEP = 0x02,
    CH32_WAKE_DEEPSLEEP = 0x04,
};

#define MICROPY_PY_MACHINE_EXTRA_GLOBALS \
    { MP_ROM_QSTR(MP_QSTR_Pin), MP_ROM_PTR(&machine_pin_type) }, \
    { MP_ROM_QSTR(MP_QSTR_DAC), MP_ROM_PTR(&machine_dac_type) }, \
    { MP_ROM_QSTR(MP_QSTR_RTC), MP_ROM_PTR(&machine_rtc_type) }, \
    { MP_ROM_QSTR(MP_QSTR_WDT), MP_ROM_PTR(&machine_wdt_type) }, \
    { MP_ROM_QSTR(MP_QSTR_Timer), MP_ROM_PTR(&machine_timer_type) }, \
    { MP_ROM_QSTR(MP_QSTR_PWRON_RESET), MP_ROM_INT(CH32_RESET_PWRON) }, \
    { MP_ROM_QSTR(MP_QSTR_HARD_RESET), MP_ROM_INT(CH32_RESET_HARD) }, \
    { MP_ROM_QSTR(MP_QSTR_WDT_RESET), MP_ROM_INT(CH32_RESET_WDT) }, \
    { MP_ROM_QSTR(MP_QSTR_SOFT_RESET), MP_ROM_INT(CH32_RESET_SOFT) }, \
    { MP_ROM_QSTR(MP_QSTR_DEEPSLEEP_RESET), MP_ROM_INT(CH32_RESET_DEEPSLEEP) }, \
    { MP_ROM_QSTR(MP_QSTR_IDLE), MP_ROM_INT(CH32_WAKE_IDLE) }, \
    { MP_ROM_QSTR(MP_QSTR_SLEEP), MP_ROM_INT(CH32_WAKE_SLEEP) }, \
    { MP_ROM_QSTR(MP_QSTR_DEEPSLEEP), MP_ROM_INT(CH32_WAKE_DEEPSLEEP) }, \
    { MP_ROM_QSTR(MP_QSTR_dht_readinto), MP_ROM_PTR(&dht_readinto_obj) },
