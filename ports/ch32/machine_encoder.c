/* machine.Encoder for the CH32H417: quadrature decoding in the timer.
 *
 * Encoder mode is a slave mode of the general-purpose timers: TI1 and TI2 --
 * the same pads and alternate functions a PWM output would use, pointed the
 * other way -- drive the counter up or down according to which of them moved
 * first. The decoding is entirely in hardware, so a fast encoder costs nothing
 * and cannot be missed by a busy interpreter.
 *
 *   phases=4  SMS=011, count on every edge of both inputs
 *   phases=2  SMS=010, count on TI1 edges only
 *
 * The documented default is 1 phase, which this hardware cannot do -- there is
 * no mode that counts one edge per quadrature cycle -- so Encoder() defaults to
 * 4 and rejects 1 rather than quietly giving four times the count.
 *
 * The counter is 16 bits and the update interrupt counts wraps, exactly as in
 * machine_counter.c, so value() spans more than a turn of a fine encoder.
 * Encoder mode reverses the counting direction on its own when the shaft does,
 * and the wrap counter follows: the direction bit is read at the moment of the
 * wrap rather than assumed.
 *
 * Pins come from the PWM table through machine_pwm_channel_af(), which is the
 * one place in this port that knows which pad is which timer's channel and is
 * already exercised by every PWM test.
 */
#include <stdbool.h>

#include "ch32h417.h"

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/runtime.h"

#include "extmod/modmachine.h"

#include "irq.h"
#include "machine_pin.h"
#include "machine_pwm.h"
#include "machine_timer.h"

typedef struct _machine_encoder_obj_t {
    mp_obj_base_t base;
    uint8_t id;                 /* timer 1-12, or 0 once deinit()ed */
    uint8_t pin_a;
    uint8_t pin_b;
    uint8_t phases;
    uint32_t filter_ns;
    /* Wraps of the 16-bit counter, signed. Only value() can allocate. */
    volatile int32_t wraps;
} machine_encoder_obj_t;

/* A GC root, like the Timer and Counter tables: the update interrupt reaches
 * the object through it and an Encoder whose Python reference has been dropped
 * is still decoding. */
#define machine_encoder_obj(id) ((machine_encoder_obj_t *)MP_STATE_PORT(machine_encoder_objs)[(id) - 1])
#define machine_encoder_set_obj(id, v) (MP_STATE_PORT(machine_encoder_objs)[(id) - 1] = (v))

const mp_obj_type_t machine_encoder_type;

static TIM_TypeDef *ch32_encoder_tim(uint8_t id) {
    static TIM_TypeDef *const tims[12] = {
        TIM1, TIM2, TIM3, TIM4, TIM5, TIM6,
        TIM7, TIM8, TIM9, TIM10, TIM11, TIM12,
    };
    return tims[id - 1];
}

static IRQn_Type ch32_encoder_irqn(uint8_t id) {
    static const IRQn_Type irqs[12] = {
        TIM1_UP_IRQn, TIM2_IRQn, TIM3_IRQn, TIM4_IRQn, TIM5_IRQn, TIM6_IRQn,
        TIM7_IRQn, TIM8_UP_IRQn, TIM9_IRQn, TIM10_IRQn, TIM11_IRQn, TIM12_IRQn,
    };
    return irqs[id - 1];
}

static void ch32_encoder_clock_enable(uint8_t id, FunctionalState state) {
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

/* Input filter for TI1 and TI2, in the same units as the ETR filter in
 * machine_counter.c: N samples at a division of the timer clock. Encoders are
 * mechanical often enough that this matters -- a switch bouncing on one phase
 * counts as motion without it. */
static uint16_t ch32_encoder_filter_bits(uint32_t ns, uint32_t timer_hz) {
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

static void ch32_encoder_init_pin(uint8_t pin, uint8_t af) {
    machine_pin_clock_enable(pin);
    RCC_HB2PeriphClockCmd(RCC_HB2Periph_AFIO, ENABLE);

    /* Floating: an encoder drives both phases, and a pull would fight it. A
     * mechanical one with open-collector outputs needs its own pull-ups. */
    GPIO_InitTypeDef gpio = { 0 };
    gpio.GPIO_Pin = MACHINE_PIN_MASK(pin);
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    gpio.GPIO_Speed = GPIO_Speed_Very_High;
    GPIO_Init(machine_pin_gpio(pin), &gpio);
    GPIO_PinAFConfig(machine_pin_gpio(pin), MACHINE_PIN_NUM(pin), af);
}

static void ch32_encoder_start(machine_encoder_obj_t *self) {
    TIM_TypeDef *tim = ch32_encoder_tim(self->id);
    uint8_t af_a, af_b;

    if (!machine_pwm_channel_af(self->pin_a, self->id, 1, &af_a)) {
        mp_raise_ValueError(MP_ERROR_TEXT("phase_a is not that timer's channel 1"));
    }
    if (!machine_pwm_channel_af(self->pin_b, self->id, 2, &af_b)) {
        mp_raise_ValueError(MP_ERROR_TEXT("phase_b is not that timer's channel 2"));
    }
    ch32_encoder_init_pin(self->pin_a, af_a);
    ch32_encoder_init_pin(self->pin_b, af_b);

    ch32_encoder_clock_enable(self->id, ENABLE);
    TIM_Cmd(tim, DISABLE);

    RCC_ClocksTypeDef clocks;
    RCC_GetClocksFreq(&clocks);
    uint16_t filter = ch32_encoder_filter_bits(self->filter_ns, clocks.HCLK_Frequency);

    TIM_TimeBaseInitTypeDef base = { 0 };
    base.TIM_Prescaler = 0;
    base.TIM_CounterMode = TIM_CounterMode_Up;
    base.TIM_Period = 0xFFFF;
    base.TIM_ClockDivision = TIM_CKD_DIV1;
    base.TIM_RepetitionCounter = 0;
    TIM_TimeBaseInit(tim, &base);

    TIM_EncoderInterfaceConfig(tim,
        self->phases == 4 ? TIM_EncoderMode_TI12 : TIM_EncoderMode_TI1,
        TIM_ICPolarity_Rising, TIM_ICPolarity_Rising);

    /* The filter is not part of TIM_EncoderInterfaceConfig(), which leaves both
     * at zero, so it goes in afterwards -- and after, not before, or the call
     * above overwrites it. */
    tim->CHCTLR1 = (tim->CHCTLR1 & ~((uint16_t)0xF0F0))
        | (filter << 4) | (filter << 12);

    TIM_SetCounter(tim, 0);
    self->wraps = 0;
    TIM_GenerateEvent(tim, TIM_EventSource_Update);
    TIM_ClearITPendingBit(tim, TIM_IT_Update);
    TIM_ClearFlag(tim, TIM_FLAG_Update);
    TIM_ITConfig(tim, TIM_IT_Update, ENABLE);
    NVIC_EnableIRQ(ch32_encoder_irqn(self->id));
    TIM_Cmd(tim, ENABLE);
}

static void ch32_encoder_stop(machine_encoder_obj_t *self) {
    TIM_TypeDef *tim = ch32_encoder_tim(self->id);
    NVIC_DisableIRQ(ch32_encoder_irqn(self->id));
    TIM_ITConfig(tim, TIM_IT_Update, DISABLE);
    TIM_Cmd(tim, DISABLE);
    tim->SMCFGR &= ~(uint16_t)0x0007;    /* leave encoder mode */
    TIM_ClearITPendingBit(tim, TIM_IT_Update);
    ch32_encoder_clock_enable(self->id, DISABLE);
}

bool machine_encoder_irq(uint8_t id) {
    machine_encoder_obj_t *self = machine_encoder_obj(id);
    if (self == NULL || self->id == 0) {
        return false;
    }
    /* Which way the wrap went is the direction bit *now*: encoder mode flips it
     * whenever the shaft reverses, so assuming a direction would count a
     * backwards wrap forwards and lose 65536 counts each time. */
    TIM_TypeDef *tim = ch32_encoder_tim(id);
    /* CTLR1 bit 4 is DIR: set means counting down. The SDK has no name for
     * it -- it only ever writes the field through TIM_CounterModeConfig. */
    self->wraps += (tim->CTLR1 & (1u << 4)) ? -1 : 1;
    return true;
}

void machine_encoder_deinit_all(void) {
    for (uint8_t id = 1; id <= 12; id++) {
        machine_encoder_obj_t *self = machine_encoder_obj(id);
        if (self != NULL) {
            ch32_encoder_stop(self);
            ch32_timer_release(id, CH32_TIMER_ENCODER);
            self->id = 0;
            machine_encoder_set_obj(id, NULL);
        }
    }
}

/* --- Python bindings --- */

static void machine_encoder_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    machine_encoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->id == 0) {
        mp_printf(print, "Encoder(deinit)");
        return;
    }
    mp_printf(print, "Encoder(%u, phase_a=Pin(%u), phase_b=Pin(%u), phases=%u)",
        self->id, self->pin_a, self->pin_b, self->phases);
}

static void machine_encoder_init_helper(machine_encoder_obj_t *self,
    size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_phase_a, ARG_phase_b, ARG_phases, ARG_filter_ns };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_phase_a,   MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_phase_b,   MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_phases,    MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 4} },
        { MP_QSTR_filter_ns, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (args[ARG_phases].u_int != 2 && args[ARG_phases].u_int != 4) {
        /* 1 is the documented default elsewhere and this hardware has no mode
         * for it; saying so is better than counting four times as fast. */
        mp_raise_ValueError(MP_ERROR_TEXT("phases must be 2 or 4"));
    }
    self->phases = (uint8_t)args[ARG_phases].u_int;
    self->filter_ns = (uint32_t)args[ARG_filter_ns].u_int;

    if (args[ARG_phase_a].u_obj != mp_const_none) {
        self->pin_a = mp_hal_get_pin_obj(args[ARG_phase_a].u_obj)->id;
    }
    if (args[ARG_phase_b].u_obj != mp_const_none) {
        self->pin_b = mp_hal_get_pin_obj(args[ARG_phase_b].u_obj)->id;
    }

    ch32_encoder_start(self);
}

static mp_obj_t machine_encoder_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_check_num(n_args, n_kw, 1, MP_OBJ_FUN_ARGS_MAX, true);

    mp_int_t id = mp_obj_get_int(all_args[0]);
    if (id < 1 || id > 12 || id == 6 || id == 7) {
        /* TIM6 and TIM7 have no capture channels at all, so no TI1 or TI2. */
        mp_raise_ValueError(MP_ERROR_TEXT("no encoder inputs on that timer"));
    }

    uint8_t held = ch32_timer_owner((uint8_t)id);
    if (held != CH32_TIMER_FREE && held != CH32_TIMER_ENCODER) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("timer %d is held by %s"), (int)id, ch32_timer_owner_name(held));
    }

    machine_encoder_obj_t *self = machine_encoder_obj(id);
    if (self == NULL) {
        self = mp_obj_malloc(machine_encoder_obj_t, &machine_encoder_type);
        machine_encoder_set_obj(id, self);
    }
    self->id = (uint8_t)id;
    self->phases = 4;
    self->filter_ns = 0;
    self->wraps = 0;

    /* Default to the first pins listed for this timer's channels 1 and 2, so
     * Encoder(3) alone works on a board wired the obvious way. */
    if (!machine_pwm_channel_pin(self->id, 1, &self->pin_a)
        || !machine_pwm_channel_pin(self->id, 2, &self->pin_b)) {
        mp_raise_ValueError(MP_ERROR_TEXT("no encoder inputs on that timer"));
    }

    ch32_timer_claim((uint8_t)id, CH32_TIMER_ENCODER);

    mp_map_t kw_args;
    mp_map_init_fixed_table(&kw_args, n_kw, all_args + n_args);
    machine_encoder_init_helper(self, n_args - 1, all_args + 1, &kw_args);
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t machine_encoder_init(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    machine_encoder_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    if (self->id == 0) {
        mp_raise_OSError(MP_EINVAL);
    }
    machine_encoder_init_helper(self, n_args - 1, args + 1, kw_args);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(machine_encoder_init_obj, 1, machine_encoder_init);

static mp_obj_t machine_encoder_deinit(mp_obj_t self_in) {
    machine_encoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->id != 0) {
        ch32_encoder_stop(self);
        ch32_timer_release(self->id, CH32_TIMER_ENCODER);
        machine_encoder_set_obj(self->id, NULL);
        self->id = 0;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_encoder_deinit_obj, machine_encoder_deinit);

/* Encoder.value([v]) -- the position, signed.
 *
 * Sampled the same way as machine.Counter's: the wrap count and the hardware
 * counter advance independently, so they are read either side of each other
 * and re-read if the wrap moved. */
static mp_obj_t machine_encoder_value(size_t n_args, const mp_obj_t *args) {
    machine_encoder_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    if (self->id == 0) {
        mp_raise_OSError(MP_EINVAL);
    }
    TIM_TypeDef *tim = ch32_encoder_tim(self->id);

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
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_encoder_value_obj, 1, 2, machine_encoder_value);

static const mp_rom_map_elem_t machine_encoder_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&machine_encoder_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&machine_encoder_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_value), MP_ROM_PTR(&machine_encoder_value_obj) },
};
static MP_DEFINE_CONST_DICT(machine_encoder_locals_dict, machine_encoder_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    machine_encoder_type,
    MP_QSTR_Encoder,
    MP_TYPE_FLAG_NONE,
    make_new, machine_encoder_make_new,
    print, machine_encoder_print,
    locals_dict, &machine_encoder_locals_dict
    );

/* void * rather than the real type: the declaration is copied into
 * genhdr/root_pointers.h, which every translation unit includes. */
MP_REGISTER_ROOT_POINTER(void *machine_encoder_objs[12]);
