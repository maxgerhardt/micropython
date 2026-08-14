/* machine.Counter for the CH32H417: a timer clocked from a pin.
 *
 * External clock mode 2 -- SMCFGR's ECE bit -- feeds the ETR input straight
 * into the counter's clock, so every edge on the pin is one count and nothing
 * in software is in the loop. That is the mode to want here: mode 1 routes the
 * same signal through the slave-mode controller and costs the trigger input,
 * and the input-capture channels cost an interrupt per pulse.
 *
 * The hardware counter is 16 bits, so the update interrupt counts wraps and
 * value() combines the two. As the documentation for machine.Counter suggests,
 * the internal state stays inside a small integer and only the value() call
 * can allocate.
 *
 * --- ETR pins ----------------------------------------------------------
 *
 * Only TIM1's is confirmed: the vendor's own Clock_Select example configures
 * PA12 as AF1 and calls TIM_ETRClockMode2Config(), which is exactly this.
 * PA12 is unusable here because it is USBFS D+.
 *
 * The rest of the table is inferred, and marked as such, because it matches
 * STM32F4 -- where TIM1_ETR is also PA12/AF1 and TIM1_CH2 is also PE11, both
 * of which this chip agrees with. That is evidence, not proof: the CAN
 * alternate functions on this part are *not* the STM32 ones, so a pin here
 * that has never been driven should be treated as a guess until it is.
 *
 * PA5 is the one that has been, and it needed no wiring to do it: the counter
 * samples the pad, so driving PA5 as an ordinary GPIO output is counted
 * exactly as an external signal would be. 2000 pulses at 128 ns apiece come
 * back as 2000, which confirms TIM2_ETR = PA5/AF1 and with it the pattern
 * behind the rest of the table.
 */
#include <stdbool.h>

#include "ch32h417.h"

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/runtime.h"

#include "extmod/modmachine.h"

#include "irq.h"
#include "machine_pin.h"
#include "machine_timer.h"

#define COUNTER_EDGE_RISING (0)
#define COUNTER_EDGE_FALLING (1)
#define COUNTER_DIR_UP (0)
#define COUNTER_DIR_DOWN (1)

typedef struct _ch32_counter_etr_t {
    uint8_t timer;
    uint8_t pin;                /* machine.Pin id, port * 16 + number */
    uint8_t af;
    bool verified;              /* driven on hardware, rather than inferred */
} ch32_counter_etr_t;

/* Which pin is which timer's ETR. See the note at the top about how much of
 * this is known and how much is inference. */
static const ch32_counter_etr_t ch32_counter_etr[] = {
    { 1, MACHINE_PIN_ID(0, 12), GPIO_AF1, true },   // PA12, vendor example (USB D+)
    { 1, MACHINE_PIN_ID(4, 7), GPIO_AF1, false },   // PE7
    { 2, MACHINE_PIN_ID(0, 5), GPIO_AF1, true },    // PA5, driven and counted
    { 2, MACHINE_PIN_ID(0, 0), GPIO_AF1, false },   // PA0
    { 2, MACHINE_PIN_ID(0, 15), GPIO_AF1, false },  // PA15
    { 3, MACHINE_PIN_ID(3, 2), GPIO_AF2, false },   // PD2
    { 4, MACHINE_PIN_ID(4, 0), GPIO_AF2, false },   // PE0
    { 8, MACHINE_PIN_ID(0, 0), GPIO_AF3, false },   // PA0
};

/* TIM6 and TIM7 are basic timers with no ETR input at all, which is why the
 * SDK's own TIM_ETRConfig() lists every timer except those two. */
static bool ch32_counter_timer_has_etr(uint8_t id) {
    return id >= 1 && id <= 12 && id != 6 && id != 7;
}

typedef struct _machine_counter_obj_t {
    mp_obj_base_t base;
    uint8_t id;                 /* timer 1-12, or 0 once deinit()ed */
    uint8_t pin;
    uint8_t edge;
    uint8_t direction;
    uint32_t filter_ns;
    /* Wraps of the 16-bit hardware counter, signed so counting down works.
     * Kept small so that only value() can allocate. */
    volatile int32_t wraps;
} machine_counter_obj_t;

/* A GC root for the same reason machine.Timer's table is one: the update
 * interrupt reaches the object through it, and a Counter whose Python
 * reference has been dropped is still counting. */
#define machine_counter_obj(id) ((machine_counter_obj_t *)MP_STATE_PORT(machine_counter_objs)[(id) - 1])
#define machine_counter_set_obj(id, v) (MP_STATE_PORT(machine_counter_objs)[(id) - 1] = (v))

const mp_obj_type_t machine_counter_type;

/* Shared with machine_timer.c, which owns the register maps. Declared here
 * rather than exported from there because the two files disagree about almost
 * nothing else and a header for three lines would be worse. */
static TIM_TypeDef *ch32_counter_tim(uint8_t id) {
    static TIM_TypeDef *const tims[12] = {
        TIM1, TIM2, TIM3, TIM4, TIM5, TIM6,
        TIM7, TIM8, TIM9, TIM10, TIM11, TIM12,
    };
    return tims[id - 1];
}

static IRQn_Type ch32_counter_irqn(uint8_t id) {
    static const IRQn_Type irqs[12] = {
        TIM1_UP_IRQn, TIM2_IRQn, TIM3_IRQn, TIM4_IRQn, TIM5_IRQn, TIM6_IRQn,
        TIM7_IRQn, TIM8_UP_IRQn, TIM9_IRQn, TIM10_IRQn, TIM11_IRQn, TIM12_IRQn,
    };
    return irqs[id - 1];
}

static void ch32_counter_clock_enable(uint8_t id, FunctionalState state) {
    static const uint32_t hb1[] = {
        RCC_HB1Periph_TIM2, RCC_HB1Periph_TIM3, RCC_HB1Periph_TIM4,
        RCC_HB1Periph_TIM5, RCC_HB1Periph_TIM6, RCC_HB1Periph_TIM7,
    };
    static const uint32_t hb2[] = {
        RCC_HB2Periph_TIM9, RCC_HB2Periph_TIM10,
        RCC_HB2Periph_TIM11, RCC_HB2Periph_TIM12,
    };
    if (id == 1) {
        RCC_HB2PeriphClockCmd(RCC_HB2Periph_TIM1, state);
    } else if (id == 8) {
        RCC_HB2PeriphClockCmd(RCC_HB2Periph_TIM8, state);
    } else if (id >= 9) {
        RCC_HB2PeriphClockCmd(hb2[id - 9], state);
    } else {
        RCC_HB1PeriphClockCmd(hb1[id - 2], state);
    }
}

/* The ETR digital filter samples the input N times before believing a change.
 * The rates available are the timer clock and its /2 and /4 divisions of the
 * dead-time generator, which at 100 MHz makes the shortest usable filter 20 ns
 * and the longest about 5 us. Returns the ETF field for the longest filter no
 * longer than the request, which is what the machine.Counter documentation
 * asks for. */
static uint16_t ch32_counter_filter_bits(uint32_t ns, uint32_t timer_hz) {
    /* Sample count and divider for each ETF value, from the reference manual's
     * table: N samples at f_DTS, where f_DTS is CK_INT / (1, 2, 4, 8, 16, 32). */
    static const struct {
        uint8_t samples;
        uint8_t log2_div;
    } table[16] = {
        { 1, 0 },  { 2, 0 },  { 4, 0 },  { 8, 0 },
        { 6, 1 },  { 8, 1 },  { 6, 2 },  { 8, 2 },
        { 6, 3 },  { 8, 3 },  { 5, 4 },  { 6, 4 },
        { 8, 4 },  { 5, 5 },  { 6, 5 },  { 8, 5 },
    };
    uint16_t best = 0;
    uint64_t best_ns = 0;
    for (uint16_t i = 0; i < 16; i++) {
        uint64_t period_ns = (1000000000ull << table[i].log2_div) / timer_hz;
        uint64_t total = period_ns * table[i].samples;
        if (total <= ns && total >= best_ns) {
            best = i;
            best_ns = total;
        }
    }
    return best;
}

static void ch32_counter_start(machine_counter_obj_t *self) {
    TIM_TypeDef *tim = ch32_counter_tim(self->id);

    machine_pin_clock_enable(self->pin);
    RCC_HB2PeriphClockCmd(RCC_HB2Periph_AFIO, ENABLE);

    /* Floating input: the signal comes from outside and a pull would fight it.
     * The mux still has to be pointed at the timer, or the pad never reaches
     * ETR -- the same F1-style detail that catches every input on this part. */
    GPIO_InitTypeDef gpio = { 0 };
    gpio.GPIO_Pin = MACHINE_PIN_MASK(self->pin);
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    gpio.GPIO_Speed = GPIO_Speed_Very_High;
    GPIO_Init(machine_pin_gpio(self->pin), &gpio);

    uint8_t af = GPIO_AF1;
    for (size_t i = 0; i < MP_ARRAY_SIZE(ch32_counter_etr); i++) {
        if (ch32_counter_etr[i].timer == self->id && ch32_counter_etr[i].pin == self->pin) {
            af = ch32_counter_etr[i].af;
            break;
        }
    }
    GPIO_PinAFConfig(machine_pin_gpio(self->pin), MACHINE_PIN_NUM(self->pin), af);

    ch32_counter_clock_enable(self->id, ENABLE);
    TIM_Cmd(tim, DISABLE);

    RCC_ClocksTypeDef clocks;
    RCC_GetClocksFreq(&clocks);

    TIM_TimeBaseInitTypeDef base = { 0 };
    base.TIM_Prescaler = 0;
    base.TIM_CounterMode = self->direction == COUNTER_DIR_DOWN
        ? TIM_CounterMode_Down : TIM_CounterMode_Up;
    /* Free-running over the full 16 bits. The wrap is what the interrupt
     * counts, so a smaller reload would only make it fire more often. */
    base.TIM_Period = 0xFFFF;
    base.TIM_ClockDivision = TIM_CKD_DIV1;
    base.TIM_RepetitionCounter = 0;
    TIM_TimeBaseInit(tim, &base);

    /* ETP straight: non-inverted counts the rising edge, inverted the falling.
     * Measured, because the vendor's Clock_Select example uses Inverted and
     * says nothing about which edge it wanted -- following it blindly gives a
     * Counter whose RISING counts falls, which a burst of square waves cannot
     * detect since it has one edge of each per cycle. test_counter.py watches
     * *when* the count moves instead. */
    TIM_ETRClockMode2Config(tim,
        TIM_ExtTRGPSC_OFF,
        self->edge == COUNTER_EDGE_RISING
            ? TIM_ExtTRGPolarity_NonInverted : TIM_ExtTRGPolarity_Inverted,
        ch32_counter_filter_bits(self->filter_ns, clocks.HCLK_Frequency));

    TIM_SetCounter(tim, 0);
    self->wraps = 0;
    TIM_GenerateEvent(tim, TIM_EventSource_Update);
    TIM_ClearITPendingBit(tim, TIM_IT_Update);
    TIM_ClearFlag(tim, TIM_FLAG_Update);
    TIM_ITConfig(tim, TIM_IT_Update, ENABLE);
    NVIC_EnableIRQ(ch32_counter_irqn(self->id));
    TIM_Cmd(tim, ENABLE);
}

static void ch32_counter_stop(machine_counter_obj_t *self) {
    TIM_TypeDef *tim = ch32_counter_tim(self->id);
    NVIC_DisableIRQ(ch32_counter_irqn(self->id));
    TIM_ITConfig(tim, TIM_IT_Update, DISABLE);
    TIM_Cmd(tim, DISABLE);
    tim->SMCFGR &= ~TIM_ECE;
    TIM_ClearITPendingBit(tim, TIM_IT_Update);
    ch32_counter_clock_enable(self->id, DISABLE);
}

/* Called from machine_timer.c's shared interrupt handlers, which own the
 * vectors: a timer is either a Timer or a Counter, never both, so one handler
 * per timer serves whichever holds it. */
bool machine_counter_irq(uint8_t id) {
    machine_counter_obj_t *self = machine_counter_obj(id);
    if (self == NULL || self->id == 0) {
        return false;
    }
    self->wraps += self->direction == COUNTER_DIR_DOWN ? -1 : 1;
    return true;
}

void machine_counter_deinit_all(void) {
    for (uint8_t id = 1; id <= 12; id++) {
        machine_counter_obj_t *self = machine_counter_obj(id);
        if (self != NULL) {
            ch32_counter_stop(self);
            ch32_timer_release(id, CH32_TIMER_COUNTER);
            self->id = 0;
            machine_counter_set_obj(id, NULL);
        }
    }
}

/* --- Python bindings --- */

static void machine_counter_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    machine_counter_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->id == 0) {
        mp_printf(print, "Counter(deinit)");
        return;
    }
    mp_printf(print, "Counter(%u, src=Pin(%u), edge=%q)", self->id, self->pin,
        self->edge == COUNTER_EDGE_RISING ? MP_QSTR_RISING : MP_QSTR_FALLING);
}

static void machine_counter_init_helper(machine_counter_obj_t *self,
    size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_src, ARG_edge, ARG_direction, ARG_filter_ns };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_src,       MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_edge,      MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = COUNTER_EDGE_RISING} },
        { MP_QSTR_direction, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = COUNTER_DIR_UP} },
        { MP_QSTR_filter_ns, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (args[ARG_edge].u_int != COUNTER_EDGE_RISING && args[ARG_edge].u_int != COUNTER_EDGE_FALLING) {
        mp_raise_ValueError(MP_ERROR_TEXT("edge must be RISING or FALLING"));
    }
    if (args[ARG_direction].u_int != COUNTER_DIR_UP && args[ARG_direction].u_int != COUNTER_DIR_DOWN) {
        mp_raise_ValueError(MP_ERROR_TEXT("direction must be UP or DOWN"));
    }
    self->edge = (uint8_t)args[ARG_edge].u_int;
    self->direction = (uint8_t)args[ARG_direction].u_int;
    self->filter_ns = (uint32_t)args[ARG_filter_ns].u_int;

    if (args[ARG_src].u_obj != mp_const_none) {
        uint8_t pin = mp_hal_get_pin_obj(args[ARG_src].u_obj)->id;
        bool ok = false;
        for (size_t i = 0; i < MP_ARRAY_SIZE(ch32_counter_etr); i++) {
            if (ch32_counter_etr[i].timer == self->id && ch32_counter_etr[i].pin == pin) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            mp_raise_ValueError(MP_ERROR_TEXT("pin is not that timer's ETR input"));
        }
        self->pin = pin;
    } else if (self->pin == 0xFF) {
        mp_raise_ValueError(MP_ERROR_TEXT("src pin required"));
    }

    ch32_counter_start(self);
}

static mp_obj_t machine_counter_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_check_num(n_args, n_kw, 1, MP_OBJ_FUN_ARGS_MAX, true);

    mp_int_t id = mp_obj_get_int(all_args[0]);
    if (!ch32_counter_timer_has_etr((uint8_t)id)) {
        mp_raise_ValueError(MP_ERROR_TEXT("no ETR input on that timer"));
    }

    uint8_t held = ch32_timer_owner((uint8_t)id);
    if (held != CH32_TIMER_FREE && held != CH32_TIMER_COUNTER) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("timer %d is held by %s"), (int)id, ch32_timer_owner_name(held));
    }

    machine_counter_obj_t *self = machine_counter_obj(id);
    if (self == NULL) {
        self = mp_obj_malloc(machine_counter_obj_t, &machine_counter_type);
        self->pin = 0xFF;
        machine_counter_set_obj(id, self);
    }
    self->id = (uint8_t)id;
    self->edge = COUNTER_EDGE_RISING;
    self->direction = COUNTER_DIR_UP;
    self->filter_ns = 0;
    self->wraps = 0;

    /* The default source is the timer's first listed ETR pin, so
     * Counter(2) alone is enough on a board that uses the obvious one. */
    if (self->pin == 0xFF) {
        for (size_t i = 0; i < MP_ARRAY_SIZE(ch32_counter_etr); i++) {
            if (ch32_counter_etr[i].timer == self->id && ch32_counter_etr[i].verified) {
                self->pin = ch32_counter_etr[i].pin;
                break;
            }
        }
    }

    ch32_timer_claim((uint8_t)id, CH32_TIMER_COUNTER);

    mp_map_t kw_args;
    mp_map_init_fixed_table(&kw_args, n_kw, all_args + n_args);
    machine_counter_init_helper(self, n_args - 1, all_args + 1, &kw_args);
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t machine_counter_init(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    machine_counter_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    if (self->id == 0) {
        mp_raise_OSError(MP_EINVAL);
    }
    machine_counter_init_helper(self, n_args - 1, args + 1, kw_args);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(machine_counter_init_obj, 1, machine_counter_init);

static mp_obj_t machine_counter_deinit(mp_obj_t self_in) {
    machine_counter_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->id != 0) {
        ch32_counter_stop(self);
        ch32_timer_release(self->id, CH32_TIMER_COUNTER);
        machine_counter_set_obj(self->id, NULL);
        self->id = 0;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_counter_deinit_obj, machine_counter_deinit);

/* Counter.value([v]) -- read, and optionally set, the count.
 *
 * The wrap count and the hardware counter are two registers advancing
 * independently, so they are sampled either side of each other and re-sampled
 * if the wrap moved: without that a read landing exactly on the wrap pairs a
 * new wrap count with an old counter and the value jumps back by 65536. */
static mp_obj_t machine_counter_value(size_t n_args, const mp_obj_t *args) {
    machine_counter_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    if (self->id == 0) {
        mp_raise_OSError(MP_EINVAL);
    }
    TIM_TypeDef *tim = ch32_counter_tim(self->id);

    int32_t wraps;
    uint16_t cnt;
    do {
        wraps = self->wraps;
        cnt = TIM_GetCounter(tim);
    } while (wraps != self->wraps);

    mp_int_t value = (mp_int_t)wraps * 65536 + cnt;

    if (n_args > 1) {
        mp_int_t set = mp_obj_get_int(args[1]);
        uint32_t state = mp_hal_atomic_enter();
        self->wraps = (int32_t)(set >> 16);
        TIM_SetCounter(tim, (uint16_t)(set & 0xFFFF));
        mp_hal_atomic_exit(state);
    }
    return mp_obj_new_int(value);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_counter_value_obj, 1, 2, machine_counter_value);

static const mp_rom_map_elem_t machine_counter_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&machine_counter_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&machine_counter_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_value), MP_ROM_PTR(&machine_counter_value_obj) },

    { MP_ROM_QSTR(MP_QSTR_RISING), MP_ROM_INT(COUNTER_EDGE_RISING) },
    { MP_ROM_QSTR(MP_QSTR_FALLING), MP_ROM_INT(COUNTER_EDGE_FALLING) },
    { MP_ROM_QSTR(MP_QSTR_UP), MP_ROM_INT(COUNTER_DIR_UP) },
    { MP_ROM_QSTR(MP_QSTR_DOWN), MP_ROM_INT(COUNTER_DIR_DOWN) },
};
static MP_DEFINE_CONST_DICT(machine_counter_locals_dict, machine_counter_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    machine_counter_type,
    MP_QSTR_Counter,
    MP_TYPE_FLAG_NONE,
    make_new, machine_counter_make_new,
    print, machine_counter_print,
    locals_dict, &machine_counter_locals_dict
    );

/* void * rather than the real type: the declaration is copied into
 * genhdr/root_pointers.h, which every translation unit includes. */
MP_REGISTER_ROOT_POINTER(void *machine_counter_objs[12]);
