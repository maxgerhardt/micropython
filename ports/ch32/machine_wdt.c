/* machine.WDT for the CH32H417, on the independent watchdog (IWDG).
 *
 * IWDG runs from the LSI, its own RC oscillator, so it keeps counting even if
 * the PLL drops out or the code that was feeding it has stopped the bus clock.
 * That independence is the point, and it costs accuracy: the datasheet gives
 * the LSI as 25-60 kHz, so a nominal timeout is only good to about a factor of
 * two across parts and temperature. On this board it measures ~39 kHz against
 * the host clock, close to the 40 kHz nominal used here -- see test_wdt.py,
 * which times a real reset and reports the frequency it implies.
 *
 * Range is 1 ms to 26 s. Once started it cannot be stopped, which is what
 * machine.WDT promises.
 *
 * The chip also has a window watchdog (WWDG), and this port deliberately does
 * not expose it. WWDG counts HCLK/4096 through seven bits, so its entire range
 * is 0.04 to 20.6 ms -- and that ceiling is hardware, not a configuration
 * choice. Measured on this board, a Python loop calling feed() every 10 ms
 * still misses the deadline, because a sleep plus the USB poll in the VM hook
 * can overshoot 20 ms. A watchdog that resets a healthy board is worse than no
 * watchdog, so offering it under machine.WDT would only invite people to try.
 */
#include "ch32h417.h"

#include "py/mphal.h"
#include "py/runtime.h"

#include "machine_sleep.h"
#include "machine_wdt.h"

/* Nominal LSI. See the file comment before trusting it to better than a factor
 * of two on an arbitrary part. */
#define WDT_LSI_HZ         (40000)

/* IWDG_RLDR is 12 bits and the prescaler runs 4, 8, ... 256. */
#define IWDG_RELOAD_MAX    (4095)
#define IWDG_PRESCALER_MIN (4)
#define IWDG_PRESCALER_MAX (256)

typedef struct _machine_wdt_obj_t {
    mp_obj_base_t base;
} machine_wdt_obj_t;

static const machine_wdt_obj_t machine_wdt_singleton = { { &machine_wdt_type } };

/* Set once the IWDG has been started. There is no way to ask the hardware:
 * IWDG_Enable() is one-way, with no disable and no status bit for "armed". */
static bool machine_wdt_running;

bool machine_wdt_is_running(void) {
    return machine_wdt_running;
}

/* Smallest prescaler that fits the timeout, because the prescaler is also the
 * resolution: at the top of the range one count is 6.4 ms. */
static void machine_wdt_start(mp_int_t timeout_ms) {
    uint32_t ticks_needed = (uint32_t)(((uint64_t)timeout_ms * WDT_LSI_HZ + 999) / 1000);
    uint32_t prescaler = IWDG_PRESCALER_MIN;
    uint8_t psc_code = IWDG_Prescaler_4;
    while (ticks_needed / prescaler > IWDG_RELOAD_MAX && prescaler < IWDG_PRESCALER_MAX) {
        prescaler *= 2;
        psc_code++;
    }
    uint32_t reload = (ticks_needed + prescaler - 1) / prescaler;
    if (reload > IWDG_RELOAD_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("timeout too long; the ceiling is about 26 s"));
    }
    if (reload == 0) {
        reload = 1;
    }

    /* The LSI is not running out of reset. IWDG counts nothing without it, so
     * the watchdog would sit there looking armed and never fire. */
    RCC_LSICmd(ENABLE);
    while (RCC_GetFlagStatus(RCC_FLAG_LSIRDY) == RESET) {
    }

    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
    IWDG_SetPrescaler(psc_code);
    IWDG_SetReload((uint16_t)reload);
    IWDG_ReloadCounter();
    IWDG_Enable();
    machine_wdt_running = true;
}

/* Same as above but reports failure instead of raising, because deepsleep()
 * calls it to pick a wake source and has a second option to fall back to. */
bool machine_wdt_start_raw(uint32_t timeout_ms) {
    uint32_t ticks_needed = (uint32_t)(((uint64_t)timeout_ms * WDT_LSI_HZ + 999) / 1000);
    uint32_t prescaler = IWDG_PRESCALER_MIN;
    uint8_t psc_code = IWDG_Prescaler_4;
    while (ticks_needed / prescaler > IWDG_RELOAD_MAX && prescaler < IWDG_PRESCALER_MAX) {
        prescaler *= 2;
        psc_code++;
    }
    uint32_t reload = (ticks_needed + prescaler - 1) / prescaler;
    if (reload > IWDG_RELOAD_MAX) {
        return false;
    }
    if (reload == 0) {
        reload = 1;
    }

    RCC_LSICmd(ENABLE);
    mp_uint_t start = mp_hal_ticks_ms();
    while (RCC_GetFlagStatus(RCC_FLAG_LSIRDY) == RESET) {
        if ((mp_uint_t)(mp_hal_ticks_ms() - start) > 100) {
            return false;
        }
    }

    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
    IWDG_SetPrescaler(psc_code);
    IWDG_SetReload((uint16_t)reload);
    IWDG_ReloadCounter();
    IWDG_Enable();
    machine_wdt_running = true;
    return true;
}

static void machine_wdt_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    (void)self_in;
    mp_printf(print, "WDT(0)");
}

enum { ARG_id, ARG_timeout };
static const mp_arg_t machine_wdt_allowed_args[] = {
    { MP_QSTR_id,      MP_ARG_INT, {.u_int = 0} },
    { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 5000} },
};

static mp_obj_t machine_wdt_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    (void)type;
    mp_arg_val_t args[MP_ARRAY_SIZE(machine_wdt_allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args,
        MP_ARRAY_SIZE(machine_wdt_allowed_args), machine_wdt_allowed_args, args);

    if (args[ARG_id].u_int != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("only WDT(0) exists"));
    }
    if (args[ARG_timeout].u_int <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("timeout must be positive"));
    }
    machine_wdt_start(args[ARG_timeout].u_int);
    return MP_OBJ_FROM_PTR(&machine_wdt_singleton);
}

// WDT.feed()
static mp_obj_t machine_wdt_feed(mp_obj_t self_in) {
    (void)self_in;
    IWDG_ReloadCounter();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_wdt_feed_obj, machine_wdt_feed);

static const mp_rom_map_elem_t machine_wdt_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_feed), MP_ROM_PTR(&machine_wdt_feed_obj) },
};
static MP_DEFINE_CONST_DICT(machine_wdt_locals_dict, machine_wdt_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    machine_wdt_type,
    MP_QSTR_WDT,
    MP_TYPE_FLAG_NONE,
    make_new, machine_wdt_make_new,
    print, machine_wdt_print,
    locals_dict, &machine_wdt_locals_dict
    );

/* --- why the board last restarted --- */

/* Latched at startup, before anything clears them. The flags survive a reset
 * and accumulate until cleared, so reading them later would report every
 * reason since power-on rather than the most recent one. */
static uint8_t machine_wdt_reset_cause_value;
static uint32_t machine_wdt_reset_flags_raw;

/* The cooked reset_cause() cannot tell every cause apart -- the V5F boots with
 * SFTRST already set, so SOFT is what most boots report regardless. This keeps
 * the raw register for when that distinction matters. */
uint32_t machine_wdt_reset_flags(void) {
    return machine_wdt_reset_flags_raw;
}

void machine_wdt_reset_cause_init(void) {
    machine_wdt_reset_flags_raw = RCC->RSTSCKR;
    /* Checked first because deepsleep is *implemented* as a software reset, so
     * SFTRST is set too and would otherwise mask it. The flag it reads lives
     * in RAM that survives a reset but not a power cycle, which is what makes
     * the distinction possible at all -- this chip has no backup registers. */
    if (machine_sleep_deepsleep_flag_take()) {
        machine_wdt_reset_cause_value = CH32_RESET_DEEPSLEEP;
    } else if (RCC_GetFlagStatus(RCC_FLAG_IWDGRST) != RESET) {
        machine_wdt_reset_cause_value = CH32_RESET_WDT;
    } else if (RCC_GetFlagStatus(RCC_FLAG_PORRST) != RESET) {
        /* Ahead of SFTRST, which is always set on this core: the V3F stub
         * starts the V5F through NVIC_WakeUp_V5F, and that leaves the software
         * reset flag behind on every boot. Testing SFTRST first made a genuine
         * power-on report SOFT_RESET, so PWRON_RESET was unreachable. The
         * flags do not accumulate across boots -- RCC_ClearFlag() below sees
         * to that -- so PORRST really does mean this boot was a power-on. */
        machine_wdt_reset_cause_value = CH32_RESET_PWRON;
    } else if (RCC_GetFlagStatus(RCC_FLAG_PINRST) != RESET) {
        /* Also ahead of SFTRST, and for the same reason PORRST is: with SFTRST
         * set on every boot, a pin reset was unreachable and every one of them
         * reported SOFT_RESET.
         *
         * That is not a cosmetic ordering point. A supply sag on a board whose
         * NRST is wired to a debug probe resets through this pin rather than
         * through the power-on detector, so an under-powered board looks
         * exactly like a software reset. Chasing one of those cost most of a
         * day during Ethernet bring-up -- the board was being powered from the
         * WCH-Link's 3V3, which could not carry the PHY under load. Reported
         * as HARD_RESET it would have been obvious.
         *
         * Safe to test before SFTRST because the V3F stub's NVIC_WakeUp_V5F
         * leaves SFTRST behind but not PINRST: measured, a normal boot reads
         * RSTSCKR = 0x10000000, SFTRST alone. */
        machine_wdt_reset_cause_value = CH32_RESET_HARD;
    } else if (RCC_GetFlagStatus(RCC_FLAG_SFTRST) != RESET) {
        machine_wdt_reset_cause_value = CH32_RESET_SOFT;
    } else {
        machine_wdt_reset_cause_value = 0;
    }
    /* Power-on sets PORRST *and* PINRST, and a software reset sets SFTRST on
     * top of whatever was already there, so the order above matters and the
     * flags have to be cleared or the next boot inherits them. */
    RCC_ClearFlag();
}

mp_int_t machine_wdt_reset_cause(void) {
    return machine_wdt_reset_cause_value;
}
