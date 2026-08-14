/* machine.Timer for the CH32H417: a real hardware timer, not a soft one.
 *
 * extmod/machine_timer.c would give machine.Timer for free, but it is
 * softtimer.c behind the scenes -- one millisecond of resolution, callbacks
 * dispatched from the scheduler, and everything sharing the SysTick. This chip
 * has twelve general-purpose timers doing nothing most of the time, so this
 * uses them: microsecond periods, an interrupt per timer, and an optional hard
 * callback that runs in the ISR.
 *
 * --- who owns which timer ----------------------------------------------
 *
 * Three things want them: machine.PWM drives the compare channels,
 * machine.Timer drives the update event, and machine.Counter clocks the
 * counter from a pin. All three set the period, so no two can share one.
 *
 * The claim table below is the arbiter, and it lives here because this is an
 * ordinary translation unit -- machine_pwm.c and machine_counter.c are pasted
 * into their extmod hosts and are static throughout, so neither can hold state
 * the others reach. PWM claims when its first channel goes up and releases
 * when its last comes down.
 *
 * Timer(-1) allocates, and prefers TIM6 and TIM7. Those are the two with no
 * output channels at all, so a Timer on one of them costs PWM nothing; taking
 * TIM3 instead would silently remove four PWM outputs.
 */
#include <stdbool.h>

#include "ch32h417.h"

#include "py/gc.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/runtime.h"

#include "extmod/modmachine.h"

#include "irq.h"
#include "machine_counter.h"
#include "machine_timer.h"

#define TIMER_MIN (1)
#define TIMER_MAX (12)
#define TIMER_COUNT (TIMER_MAX - TIMER_MIN + 1)

#define TIMER_MODE_ONE_SHOT (0)
#define TIMER_MODE_PERIODIC (1)

/* Both the prescaler and the reload are 16 bit on the timers this treats
 * uniformly, so the longest period is 65536 * 65536 / HCLK -- about 43 s at
 * 100 MHz -- and the shortest useful one is a few microseconds. */
#define TIMER_DIV_MAX (65536u)

typedef struct _machine_timer_obj_t {
    mp_obj_base_t base;
    uint8_t id;                     /* 1-12, or 0 once deinit()ed */
    uint8_t mode;
    bool hard;
    uint32_t period_us;
    mp_obj_t callback;
} machine_timer_obj_t;

/* Indexed by timer number minus one, and a GC root.
 *
 * A root because the object is the only thing the interrupt can reach the
 * callback through, and nothing else need refer to it: `machine.Timer(6,
 * freq=10)` with the result thrown away still has a running timer whose
 * callback must not be collected. It is cleared by machine_timer_deinit_all()
 * on soft reset, before the heap it points into is reclaimed. */
#define machine_timer_obj(id) ((machine_timer_obj_t *)MP_STATE_PORT(machine_timer_objs)[(id) - 1])
#define machine_timer_set_obj(id, v) (MP_STATE_PORT(machine_timer_objs)[(id) - 1] = (v))

static TIM_TypeDef *machine_timer_tim(uint8_t id) {
    static TIM_TypeDef *const tims[TIMER_COUNT] = {
        TIM1, TIM2, TIM3, TIM4, TIM5, TIM6,
        TIM7, TIM8, TIM9, TIM10, TIM11, TIM12,
    };
    return tims[id - 1];
}

/* The update interrupt. TIM1 and TIM8 are the advanced timers and split theirs
 * across four vectors; everything else has one. */
static IRQn_Type machine_timer_irqn(uint8_t id) {
    static const IRQn_Type irqs[TIMER_COUNT] = {
        TIM1_UP_IRQn, TIM2_IRQn, TIM3_IRQn, TIM4_IRQn, TIM5_IRQn, TIM6_IRQn,
        TIM7_IRQn, TIM8_UP_IRQn, TIM9_IRQn, TIM10_IRQn, TIM11_IRQn, TIM12_IRQn,
    };
    return irqs[id - 1];
}

/* --- the shared claim table ---
 *
 * See machine_timer.h. Plain static rather than a GC root: it holds owner tags,
 * not pointers. */
static uint8_t ch32_timer_claims[TIMER_COUNT];

bool ch32_timer_claim(uint8_t id, uint8_t owner) {
    if (id < TIMER_MIN || id > TIMER_MAX) {
        return false;
    }
    uint8_t held = ch32_timer_claims[id - 1];
    if (held != CH32_TIMER_FREE && held != owner) {
        return false;
    }
    ch32_timer_claims[id - 1] = owner;
    return true;
}

void ch32_timer_release(uint8_t id, uint8_t owner) {
    if (id >= TIMER_MIN && id <= TIMER_MAX && ch32_timer_claims[id - 1] == owner) {
        ch32_timer_claims[id - 1] = CH32_TIMER_FREE;
    }
}

uint8_t ch32_timer_owner(uint8_t id) {
    if (id < TIMER_MIN || id > TIMER_MAX) {
        return CH32_TIMER_FREE;
    }
    return ch32_timer_claims[id - 1];
}

const char *ch32_timer_owner_name(uint8_t owner) {
    switch (owner) {
        case CH32_TIMER_PWM:
            return "PWM";
        case CH32_TIMER_TIMER:
            return "Timer";
        case CH32_TIMER_COUNTER:
            return "Counter";
        default:
            return "nothing";
    }
}

/* --- clocks ---
 *
 * Same split as PWM: TIM1, TIM8 and TIM9-TIM12 are on HB2, TIM2-TIM7 on HB1.
 * Duplicated rather than shared for the reason in the file comment -- there is
 * no non-static function in machine_pwm.c to call. */
static bool machine_timer_rcc_bit(uint8_t id, uint32_t *mask) {
    static const uint32_t hb1[] = {
        RCC_HB1Periph_TIM2, RCC_HB1Periph_TIM3, RCC_HB1Periph_TIM4,
        RCC_HB1Periph_TIM5, RCC_HB1Periph_TIM6, RCC_HB1Periph_TIM7,
    };
    static const uint32_t hb2[] = {
        RCC_HB2Periph_TIM9, RCC_HB2Periph_TIM10,
        RCC_HB2Periph_TIM11, RCC_HB2Periph_TIM12,
    };
    if (id == 1) {
        *mask = RCC_HB2Periph_TIM1;
    } else if (id == 8) {
        *mask = RCC_HB2Periph_TIM8;
    } else if (id >= 9) {
        *mask = hb2[id - 9];
    } else {
        *mask = hb1[id - 2];
        return false;
    }
    return true;
}

static void machine_timer_clock_enable(uint8_t id, FunctionalState state) {
    uint32_t mask;
    if (machine_timer_rcc_bit(id, &mask)) {
        RCC_HB2PeriphClockCmd(mask, state);
    } else {
        RCC_HB1PeriphClockCmd(mask, state);
    }
}

static uint32_t machine_timer_source_hz(void) {
    RCC_ClocksTypeDef clocks;
    RCC_GetClocksFreq(&clocks);
    return clocks.HCLK_Frequency;
}

/* Split a period into prescaler and reload.
 *
 * Prefers the smallest prescaler that keeps the reload in range, so short
 * periods keep the full resolution of the source clock: at 100 MHz a 1 ms
 * period counts to 100000, which needs a prescaler of 2, and the result is
 * exact to 20 ns rather than to whatever a larger prescaler would round to. */
static bool machine_timer_timing_for(uint32_t period_us, uint16_t *psc_out, uint16_t *arr_out) {
    uint32_t hz = machine_timer_source_hz();
    /* Ticks of the source clock in the requested period, in 64 bits because
     * 43 s at 100 MHz overflows a 32-bit product long before the timer runs
     * out of range. */
    uint64_t ticks = ((uint64_t)hz * period_us) / 1000000ull;
    if (ticks == 0) {
        return false;
    }
    uint32_t psc = (uint32_t)((ticks + TIMER_DIV_MAX - 1) / TIMER_DIV_MAX);
    if (psc > TIMER_DIV_MAX) {
        return false;
    }
    if (psc == 0) {
        psc = 1;
    }
    uint32_t arr = (uint32_t)(ticks / psc);
    if (arr == 0) {
        arr = 1;
    }
    if (arr > TIMER_DIV_MAX) {
        arr = TIMER_DIV_MAX;
    }
    *psc_out = (uint16_t)(psc - 1);
    *arr_out = (uint16_t)(arr - 1);
    return true;
}

static void machine_timer_start(machine_timer_obj_t *self) {
    TIM_TypeDef *tim = machine_timer_tim(self->id);
    uint16_t psc, arr;

    if (!machine_timer_timing_for(self->period_us, &psc, &arr)) {
        mp_raise_ValueError(MP_ERROR_TEXT("period out of range"));
    }

    machine_timer_clock_enable(self->id, ENABLE);
    TIM_Cmd(tim, DISABLE);

    TIM_TimeBaseInitTypeDef base = { 0 };
    base.TIM_Prescaler = psc;
    base.TIM_CounterMode = TIM_CounterMode_Up;
    base.TIM_Period = arr;
    base.TIM_ClockDivision = TIM_CKD_DIV1;
    base.TIM_RepetitionCounter = 0;
    TIM_TimeBaseInit(tim, &base);

    /* Generate an update to load the prescaler, then clear the flag it sets.
     * Without the clear the interrupt fires immediately on enable, so a
     * one-shot would run at once and a periodic would be one tick early. */
    TIM_GenerateEvent(tim, TIM_EventSource_Update);
    TIM_ClearITPendingBit(tim, TIM_IT_Update);
    TIM_ClearFlag(tim, TIM_FLAG_Update);

    TIM_ITConfig(tim, TIM_IT_Update, ENABLE);
    NVIC_EnableIRQ(machine_timer_irqn(self->id));
    TIM_Cmd(tim, ENABLE);
}

static void machine_timer_stop(uint8_t id) {
    TIM_TypeDef *tim = machine_timer_tim(id);
    NVIC_DisableIRQ(machine_timer_irqn(id));
    TIM_ITConfig(tim, TIM_IT_Update, DISABLE);
    TIM_Cmd(tim, DISABLE);
    TIM_ClearITPendingBit(tim, TIM_IT_Update);
    machine_timer_clock_enable(id, DISABLE);
}

static void machine_timer_isr(uint8_t id) {
    TIM_TypeDef *tim = machine_timer_tim(id);
    if (TIM_GetITStatus(tim, TIM_IT_Update) == RESET) {
        return;
    }
    TIM_ClearITPendingBit(tim, TIM_IT_Update);

    /* The vectors are per timer, not per driver, so a Counter's wrap arrives
     * here too. It owns the timer exclusively, so if it takes the interrupt
     * there is no Timer to consider. */
    if (machine_counter_irq(id)) {
        return;
    }

    machine_timer_obj_t *self = machine_timer_obj(id);
    if (self == NULL) {
        /* Nobody owns this any more: silence it rather than taking the
         * interrupt again forever. */
        machine_timer_stop(id);
        return;
    }

    if (self->mode == TIMER_MODE_ONE_SHOT) {
        TIM_Cmd(tim, DISABLE);
        TIM_ITConfig(tim, TIM_IT_Update, DISABLE);
    }

    if (self->callback == mp_const_none) {
        return;
    }
    if (self->hard) {
        /* Straight from the interrupt. The callback must not allocate, and
         * mp_sched_lock() keeps it from being preempted by the scheduler
         * halfway through.
         *
         * The exception path is caught here rather than left to
         * mp_call_function_1_protected(), which would print the traceback from
         * inside the interrupt. Printing goes through TinyUSB, which is not
         * interrupt-safe, and a callback that raises every time -- a hard one
         * that allocates, say -- repeats at the timer's rate: at 500 Hz that
         * starves tud_task() until its event FIFO fills, TU_ASSERT executes an
         * ebreak, and the board wedges in the SDK's weak Break_Point_Handler
         * with no output at all.
         *
         * So the callback is disabled, which is what every port does with a
         * raising hard IRQ, and the exception is handed to the main thread to
         * report at a moment when printing is safe. */
        mp_sched_lock();
        gc_lock();
        nlr_buf_t nlr;
        if (nlr_push(&nlr) == 0) {
            mp_call_function_1(self->callback, MP_OBJ_FROM_PTR(self));
            nlr_pop();
        } else {
            self->callback = mp_const_none;
            mp_sched_exception(MP_OBJ_FROM_PTR(nlr.ret_val));
        }
        gc_unlock();
        mp_sched_unlock();
    } else {
        mp_sched_schedule(self->callback, MP_OBJ_FROM_PTR(self));
    }
}

#define CH32_TIMER_HANDLER(name, id)      \
    void CH32_IRQ_HANDLER(name);          \
    void name(void) {                     \
        machine_timer_isr(id);            \
    }

CH32_TIMER_HANDLER(TIM1_UP_IRQHandler, 1)
CH32_TIMER_HANDLER(TIM2_IRQHandler, 2)
CH32_TIMER_HANDLER(TIM3_IRQHandler, 3)
CH32_TIMER_HANDLER(TIM4_IRQHandler, 4)
CH32_TIMER_HANDLER(TIM5_IRQHandler, 5)
CH32_TIMER_HANDLER(TIM6_IRQHandler, 6)
CH32_TIMER_HANDLER(TIM7_IRQHandler, 7)
CH32_TIMER_HANDLER(TIM8_UP_IRQHandler, 8)
CH32_TIMER_HANDLER(TIM9_IRQHandler, 9)
CH32_TIMER_HANDLER(TIM10_IRQHandler, 10)
CH32_TIMER_HANDLER(TIM11_IRQHandler, 11)
CH32_TIMER_HANDLER(TIM12_IRQHandler, 12)

void machine_timer_deinit_all(void) {
    for (uint8_t id = TIMER_MIN; id <= TIMER_MAX; id++) {
        machine_timer_obj_t *self = machine_timer_obj(id);
        if (self != NULL) {
            machine_timer_stop(id);
            ch32_timer_release(id, CH32_TIMER_TIMER);
            self->id = 0;
            machine_timer_set_obj(id, NULL);
        }
    }
}

/* --- Python bindings --- */

static void machine_timer_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    machine_timer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->id == 0) {
        mp_printf(print, "Timer(deinit)");
        return;
    }
    mp_printf(print, "Timer(%u, mode=%q, period=%u us)", self->id,
        self->mode == TIMER_MODE_ONE_SHOT ? MP_QSTR_ONE_SHOT : MP_QSTR_PERIODIC,
        (unsigned)self->period_us);
}

static void machine_timer_init_helper(machine_timer_obj_t *self,
    size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_mode, ARG_callback, ARG_period, ARG_tick_hz, ARG_freq, ARG_hard };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_mode,     MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = TIMER_MODE_PERIODIC} },
        { MP_QSTR_callback, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_period,   MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_tick_hz,  MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 1000} },
        { MP_QSTR_freq,     MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_hard,     MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (args[ARG_mode].u_int != TIMER_MODE_ONE_SHOT && args[ARG_mode].u_int != TIMER_MODE_PERIODIC) {
        mp_raise_ValueError(MP_ERROR_TEXT("mode must be ONE_SHOT or PERIODIC"));
    }
    self->mode = (uint8_t)args[ARG_mode].u_int;
    self->hard = args[ARG_hard].u_bool;
    self->callback = args[ARG_callback].u_obj;

    /* freq wins if both are given, matching every other port. It is taken as a
     * float so that fractional rates below 1 Hz are expressible -- the
     * hardware reaches about 0.024 Hz and refusing to say so would be an
     * artificial limit. */
    if (args[ARG_freq].u_obj != mp_const_none) {
        #if MICROPY_PY_BUILTINS_FLOAT
        mp_float_t freq = mp_obj_get_float(args[ARG_freq].u_obj);
        if (freq <= 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("freq must be positive"));
        }
        mp_float_t us = 1000000 / freq;
        #else
        mp_int_t freq = mp_obj_get_int(args[ARG_freq].u_obj);
        if (freq <= 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("freq must be positive"));
        }
        mp_int_t us = 1000000 / freq;
        #endif
        if (us < 1 || us > (mp_float_t)0xFFFFFFFFu) {
            mp_raise_ValueError(MP_ERROR_TEXT("freq out of range"));
        }
        self->period_us = (uint32_t)us;
    } else if (args[ARG_period].u_int >= 0) {
        /* period is in units of 1/tick_hz, so the default tick_hz of 1000
         * makes it milliseconds, which is what the documentation describes. */
        mp_int_t tick_hz = args[ARG_tick_hz].u_int;
        if (tick_hz <= 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("tick_hz must be positive"));
        }
        uint64_t us = ((uint64_t)args[ARG_period].u_int * 1000000ull) / (uint64_t)tick_hz;
        if (us == 0 || us > 0xFFFFFFFFu) {
            mp_raise_ValueError(MP_ERROR_TEXT("period out of range"));
        }
        self->period_us = (uint32_t)us;
    } else if (self->period_us == 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("period or freq required"));
    }

    machine_timer_start(self);
}

static mp_obj_t machine_timer_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_check_num(n_args, n_kw, 0, MP_OBJ_FUN_ARGS_MAX, true);

    mp_int_t id = n_args > 0 ? mp_obj_get_int(all_args[0]) : -1;

    if (id == -1) {
        /* TIM6 and TIM7 first: they are the two with no output channels, so a
         * Timer there costs machine.PWM nothing. */
        static const uint8_t order[TIMER_COUNT] = { 6, 7, 12, 11, 10, 9, 5, 4, 3, 2, 8, 1 };
        id = 0;
        for (size_t i = 0; i < TIMER_COUNT; i++) {
            uint8_t candidate = order[i];
            if (ch32_timer_owner(candidate) == CH32_TIMER_FREE) {
                id = candidate;
                break;
            }
        }
        if (id == 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("no timer free"));
        }
    } else if (id < TIMER_MIN || id > TIMER_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("Timer(1) to Timer(12), or Timer(-1)"));
    }

    uint8_t held = ch32_timer_owner((uint8_t)id);
    if (held != CH32_TIMER_FREE && held != CH32_TIMER_TIMER) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("timer %d is held by %s"), (int)id, ch32_timer_owner_name(held));
    }
    ch32_timer_claim((uint8_t)id, CH32_TIMER_TIMER);

    machine_timer_obj_t *self = machine_timer_obj(id);
    if (self == NULL) {
        self = mp_obj_malloc(machine_timer_obj_t, &machine_timer_type);
        machine_timer_set_obj(id, self);
    }
    self->id = (uint8_t)id;
    self->mode = TIMER_MODE_PERIODIC;
    self->hard = false;
    self->period_us = 0;
    self->callback = mp_const_none;

    if (n_args > 1 || n_kw > 0) {
        mp_map_t kw_args;
        mp_map_init_fixed_table(&kw_args, n_kw, all_args + n_args);
        machine_timer_init_helper(self, n_args - 1, all_args + 1, &kw_args);
    }
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t machine_timer_init(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    machine_timer_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    if (self->id == 0) {
        mp_raise_OSError(MP_EINVAL);
    }
    machine_timer_init_helper(self, n_args - 1, args + 1, kw_args);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(machine_timer_init_obj, 1, machine_timer_init);

static mp_obj_t machine_timer_deinit(mp_obj_t self_in) {
    machine_timer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->id != 0) {
        machine_timer_stop(self->id);
        ch32_timer_release(self->id, CH32_TIMER_TIMER);
        machine_timer_set_obj(self->id, NULL);
        self->id = 0;
        self->callback = mp_const_none;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_timer_deinit_obj, machine_timer_deinit);

/* The live counter, in microseconds since the period last rolled over. Not in
 * the documented API, but this is a hardware timer and the value is right
 * there -- it is the cheapest way for a test to prove the period is what was
 * asked for rather than trusting the callback rate. */
static mp_obj_t machine_timer_counter(mp_obj_t self_in) {
    machine_timer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->id == 0) {
        mp_raise_OSError(MP_EINVAL);
    }
    TIM_TypeDef *tim = machine_timer_tim(self->id);
    uint64_t ticks = (uint64_t)TIM_GetCounter(tim) * (TIM_GetPrescaler(tim) + 1);
    return mp_obj_new_int_from_uint((mp_uint_t)(ticks * 1000000ull / machine_timer_source_hz()));
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_timer_counter_obj, machine_timer_counter);

static const mp_rom_map_elem_t machine_timer_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&machine_timer_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&machine_timer_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_counter), MP_ROM_PTR(&machine_timer_counter_obj) },

    { MP_ROM_QSTR(MP_QSTR_ONE_SHOT), MP_ROM_INT(TIMER_MODE_ONE_SHOT) },
    { MP_ROM_QSTR(MP_QSTR_PERIODIC), MP_ROM_INT(TIMER_MODE_PERIODIC) },
};
static MP_DEFINE_CONST_DICT(machine_timer_locals_dict, machine_timer_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    machine_timer_type,
    MP_QSTR_Timer,
    MP_TYPE_FLAG_NONE,
    make_new, machine_timer_make_new,
    print, machine_timer_print,
    locals_dict, &machine_timer_locals_dict
    );

/* void * rather than the real type: the declaration is copied into
 * genhdr/root_pointers.h, which every translation unit includes. */
MP_REGISTER_ROOT_POINTER(void *machine_timer_objs[12]);
