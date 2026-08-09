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
#include "machine_wdt.h"

/* --- module-level hooks required by extmod/modmachine.c --- */

static void mp_machine_idle(void) {
    __asm volatile ("wfi");
}

static mp_obj_t mp_machine_unique_id(void) {
    /* 96-bit factory-programmed device ID. */
    const uint8_t *id = (const uint8_t *)0x1FFFF7E8;
    return mp_obj_new_bytes(id, 12);
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
    if (n_args != 0) {
        mp_hal_delay_ms(mp_obj_get_int(args[0]));
    } else {
        __asm volatile ("wfi");
    }
}

MP_NORETURN static void mp_machine_deepsleep(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    (void)args;
    NVIC_SystemReset();
    for (;;) {
    }
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

#define MICROPY_PY_MACHINE_EXTRA_GLOBALS \
    { MP_ROM_QSTR(MP_QSTR_Pin), MP_ROM_PTR(&machine_pin_type) }, \
    { MP_ROM_QSTR(MP_QSTR_DAC), MP_ROM_PTR(&machine_dac_type) }, \
    { MP_ROM_QSTR(MP_QSTR_WDT), MP_ROM_PTR(&machine_wdt_type) }, \
    { MP_ROM_QSTR(MP_QSTR_PWRON_RESET), MP_ROM_INT(CH32_RESET_PWRON) }, \
    { MP_ROM_QSTR(MP_QSTR_HARD_RESET), MP_ROM_INT(CH32_RESET_HARD) }, \
    { MP_ROM_QSTR(MP_QSTR_WDT_RESET), MP_ROM_INT(CH32_RESET_WDT) }, \
    { MP_ROM_QSTR(MP_QSTR_SOFT_RESET), MP_ROM_INT(CH32_RESET_SOFT) }, \
    { MP_ROM_QSTR(MP_QSTR_dht_readinto), MP_ROM_PTR(&dht_readinto_obj) },
