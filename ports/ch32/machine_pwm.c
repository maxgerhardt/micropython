/* machine.PWM for the CH32H417.
 *
 * Included by extmod/machine_pwm.c through MICROPY_PY_MACHINE_PWM_INCLUDEFILE,
 * so the mp_machine_pwm_* functions below must keep the static linkage that
 * file declares for them.
 *
 * Twelve timers drive the output-compare pins: TIM1 and TIM8 are the advanced
 * pair, TIM2-TIM5 are the general-purpose 32-bit ones, and TIM9-TIM12 are the
 * small ones -- which on this part have four channels each, unlike their
 * STM32 namesakes. TIM6 and TIM7 have no outputs at all and never appear here.
 *
 * Every channel runs in PWM mode 1 with the compare register preloaded, so a
 * duty change takes effect at the next update event rather than mid-pulse.
 *
 * The period lives in the timer, not the channel, so all four channels of one
 * timer share a frequency -- setting freq() on one moves the others. That is
 * the hardware, and the same caveat every other port carries. What this driver
 * adds is that changing the frequency rescales the other channels' compare
 * registers from their requested duty, so a shared timer does not silently
 * turn a 50 % channel into a 5 % one.
 *
 * Pin assignment is not fixed: most pins reach two or three different timers,
 * and the driver picks one for you (see machine_pwm_choose). Pass timer=N to
 * override, which is what you want when two outputs need unrelated
 * frequencies.
 */
#include "ch32h417.h"

#include "py/mphal.h"
#include "py/runtime.h"

#include "machine_pin.h"
#include "machine_pwm.h"

#define PWM_DEFAULT_FREQ (1000)

/* Reset value of the auto-reload register, and the largest period the compare
 * registers can express: a fully-on channel needs CCR > ARR, and CCR is 16
 * bits, so the counter has to stop one short of its own range. */
#define PWM_PERIOD_MAX   (65535)

/* Where each pin's output-compare functions live.
 *
 * Generated from the datasheet's pin table (Table 2-1-1) for the QFN128
 * package, which is the only one this port builds for. Sorted by pin, and
 * within a pin the plain channels come before the complementary ones so that
 * automatic selection prefers the straightforward output.
 *
 * "negated" marks a TIMx_CHyN pin. Those are the complementary half of a
 * channel, meant for driving the low side of a half bridge, and they carry the
 * inverse of the channel's waveform. The driver flips the complementary
 * polarity bit so that duty_u16 still means the fraction of time the pin is
 * high, which is what someone writing PWM(Pin("PE8"), ...) expects; use
 * invert=True to get the other sense. A CHyN pin shares its compare register
 * with the matching CHy pin, so the two cannot carry different duties.
 *
 * Supply domains matter here as much as they do for I2C and SPI: only some
 * pins are on the 3.3 V rail (PA5-PA7 and PE2-PE6 among them), the rest sit on
 * VIO18 and idle well below 3.3 V. See the README.
 */
typedef struct _machine_pwm_af_t {
    uint8_t pin;
    uint8_t timer;      /* 1-12 */
    uint8_t channel;    /* 1-4 */
    bool negated;       /* the pin is TIMx_CHyN, not TIMx_CHy */
    uint8_t af;
} machine_pwm_af_t;

#define PIN_ID(port, num) (((port) << 4) | (num))
#define PORT_A (0)
#define PORT_B (1)
#define PORT_C (2)
#define PORT_D (3)
#define PORT_E (4)
#define PORT_F (5)

static const machine_pwm_af_t machine_pwm_af_options[] = {
    { PIN_ID(PORT_A, 0),   2, 1, false,  1 },
    { PIN_ID(PORT_A, 0),   5, 1, false,  2 },
    { PIN_ID(PORT_A, 0),   9, 1, false,  6 },
    { PIN_ID(PORT_A, 1),   2, 2, false,  1 },
    { PIN_ID(PORT_A, 1),   5, 2, false,  2 },
    { PIN_ID(PORT_A, 1),   9, 2, false,  6 },
    { PIN_ID(PORT_A, 2),   2, 3, false,  1 },
    { PIN_ID(PORT_A, 2),   5, 3, false,  2 },
    { PIN_ID(PORT_A, 2),   9, 3, false,  4 },
    { PIN_ID(PORT_A, 3),   2, 4, false,  1 },
    { PIN_ID(PORT_A, 3),   5, 4, false,  2 },
    { PIN_ID(PORT_A, 3),   9, 4, false,  4 },
    { PIN_ID(PORT_A, 3),  10, 3, false,  8 },
    { PIN_ID(PORT_A, 4),  10, 4, false,  9 },
    { PIN_ID(PORT_A, 5),   2, 1, false,  1 },
    { PIN_ID(PORT_A, 5),   8, 1, true,   3 },
    { PIN_ID(PORT_A, 6),   3, 1, false,  2 },
    { PIN_ID(PORT_A, 6),  10, 1, false,  9 },
    { PIN_ID(PORT_A, 7),   3, 2, false,  2 },
    { PIN_ID(PORT_A, 7),  10, 2, false,  9 },
    { PIN_ID(PORT_A, 7),   1, 1, true,   1 },
    { PIN_ID(PORT_A, 7),   8, 1, true,   3 },
    { PIN_ID(PORT_A, 8),   1, 1, false,  1 },
    { PIN_ID(PORT_A, 9),   1, 2, false,  1 },   /* also USART1 TX, the REPL */
    { PIN_ID(PORT_A, 10),  1, 3, false,  1 },   /* also USART1 RX, the REPL */
    { PIN_ID(PORT_A, 11),  1, 4, false,  1 },   /* also USB D- */
    { PIN_ID(PORT_A, 15),  2, 1, false,  1 },

    { PIN_ID(PORT_B, 0),   3, 3, false,  2 },
    { PIN_ID(PORT_B, 0),   5, 4, false,  4 },
    { PIN_ID(PORT_B, 0),   1, 2, true,   1 },
    { PIN_ID(PORT_B, 0),   8, 2, true,   3 },
    { PIN_ID(PORT_B, 1),   3, 4, false,  2 },
    { PIN_ID(PORT_B, 1),  12, 1, false,  5 },
    { PIN_ID(PORT_B, 1),   1, 3, true,   1 },
    { PIN_ID(PORT_B, 1),   8, 3, true,   3 },
    { PIN_ID(PORT_B, 2),  12, 2, false,  5 },
    { PIN_ID(PORT_B, 3),   2, 2, false,  1 },
    { PIN_ID(PORT_B, 4),   3, 1, false,  2 },
    { PIN_ID(PORT_B, 5),   3, 2, false,  2 },
    { PIN_ID(PORT_B, 6),   4, 1, false,  2 },
    { PIN_ID(PORT_B, 6),  10, 1, false,  0 },
    { PIN_ID(PORT_B, 7),   4, 2, false,  2 },
    { PIN_ID(PORT_B, 7),  10, 2, false,  0 },
    { PIN_ID(PORT_B, 8),   4, 3, false,  2 },
    { PIN_ID(PORT_B, 8),  10, 3, false,  1 },
    { PIN_ID(PORT_B, 9),   4, 4, false,  2 },
    { PIN_ID(PORT_B, 9),  10, 4, false,  1 },
    { PIN_ID(PORT_B, 10),  2, 3, false,  1 },
    { PIN_ID(PORT_B, 10),  9, 2, false,  2 },
    { PIN_ID(PORT_B, 11),  2, 4, false,  1 },
    { PIN_ID(PORT_B, 11),  9, 4, false,  9 },
    { PIN_ID(PORT_B, 12),  9, 3, false,  8 },
    { PIN_ID(PORT_B, 13),  1, 1, true,   1 },
    { PIN_ID(PORT_B, 14),  9, 1, false,  2 },
    { PIN_ID(PORT_B, 14),  1, 2, true,   1 },
    { PIN_ID(PORT_B, 14),  8, 2, true,   3 },
    { PIN_ID(PORT_B, 15),  9, 2, false,  2 },
    { PIN_ID(PORT_B, 15),  1, 3, true,   1 },
    { PIN_ID(PORT_B, 15),  8, 3, true,   3 },

    { PIN_ID(PORT_C, 1),   5, 1, false,  2 },
    { PIN_ID(PORT_C, 1),   8, 1, true,   0 },
    { PIN_ID(PORT_C, 2),   5, 2, false,  2 },
    { PIN_ID(PORT_C, 2),   8, 2, true,   0 },
    { PIN_ID(PORT_C, 3),   5, 3, false,  2 },
    { PIN_ID(PORT_C, 3),   8, 3, true,   0 },
    { PIN_ID(PORT_C, 6),   3, 1, false,  2 },
    { PIN_ID(PORT_C, 6),   8, 1, false,  3 },
    { PIN_ID(PORT_C, 7),   3, 2, false,  2 },
    { PIN_ID(PORT_C, 7),   8, 2, false,  3 },
    { PIN_ID(PORT_C, 8),   3, 3, false,  2 },
    { PIN_ID(PORT_C, 8),   8, 3, false,  3 },
    { PIN_ID(PORT_C, 9),   3, 4, false,  2 },
    { PIN_ID(PORT_C, 9),   8, 4, false,  3 },
    { PIN_ID(PORT_C, 9),   9, 1, false,  6 },
    { PIN_ID(PORT_C, 11),  9, 4, false,  2 },
    { PIN_ID(PORT_C, 12),  9, 3, false,  2 },

    { PIN_ID(PORT_D, 3),  11, 1, false,  2 },
    { PIN_ID(PORT_D, 4),   3, 2, false,  9 },
    { PIN_ID(PORT_D, 4),  11, 2, false,  2 },
    { PIN_ID(PORT_D, 5),   3, 3, false,  9 },
    { PIN_ID(PORT_D, 5),  11, 3, false,  2 },
    { PIN_ID(PORT_D, 6),   3, 4, false,  9 },
    { PIN_ID(PORT_D, 6),  11, 4, false,  2 },
    { PIN_ID(PORT_D, 7),  11, 3, false, 13 },
    { PIN_ID(PORT_D, 12),  4, 1, false,  2 },
    { PIN_ID(PORT_D, 12),  5, 1, false,  6 },
    { PIN_ID(PORT_D, 13),  4, 2, false,  2 },
    { PIN_ID(PORT_D, 13),  5, 2, false,  6 },
    { PIN_ID(PORT_D, 14),  4, 3, false,  2 },
    { PIN_ID(PORT_D, 14),  5, 3, false,  6 },
    { PIN_ID(PORT_D, 15),  4, 4, false,  2 },
    { PIN_ID(PORT_D, 15),  5, 4, false,  6 },

    { PIN_ID(PORT_E, 0),  11, 1, false, 13 },
    { PIN_ID(PORT_E, 1),  11, 2, false, 13 },
    { PIN_ID(PORT_E, 3),   4, 1, false,  2 },
    { PIN_ID(PORT_E, 3),   8, 1, false,  0 },
    { PIN_ID(PORT_E, 3),  12, 1, false,  3 },
    { PIN_ID(PORT_E, 4),   4, 2, false,  2 },
    { PIN_ID(PORT_E, 4),   8, 2, false,  0 },
    { PIN_ID(PORT_E, 4),  12, 2, false,  3 },
    { PIN_ID(PORT_E, 5),   4, 3, false,  2 },
    { PIN_ID(PORT_E, 5),   8, 3, false,  0 },
    { PIN_ID(PORT_E, 5),   9, 3, false,  4 },
    { PIN_ID(PORT_E, 5),  12, 3, false,  3 },
    { PIN_ID(PORT_E, 6),   4, 4, false,  2 },
    { PIN_ID(PORT_E, 6),   8, 4, false,  0 },
    { PIN_ID(PORT_E, 6),   9, 4, false,  4 },
    { PIN_ID(PORT_E, 6),  12, 4, false,  3 },
    { PIN_ID(PORT_E, 8),   1, 1, true,   1 },
    { PIN_ID(PORT_E, 9),   1, 1, false,  1 },
    { PIN_ID(PORT_E, 10),  1, 2, true,   1 },
    { PIN_ID(PORT_E, 11),  1, 2, false,  1 },
    { PIN_ID(PORT_E, 12),  1, 3, true,   1 },
    { PIN_ID(PORT_E, 13),  1, 3, false,  1 },
    { PIN_ID(PORT_E, 13), 12, 2, false,  2 },
    { PIN_ID(PORT_E, 14),  1, 4, false,  1 },
    { PIN_ID(PORT_E, 14), 12, 3, false,  2 },
    { PIN_ID(PORT_E, 15), 12, 4, false,  2 },

    { PIN_ID(PORT_F, 6),  10, 3, false,  9 },
    { PIN_ID(PORT_F, 6),  11, 1, false, 13 },
    { PIN_ID(PORT_F, 7),  10, 4, false,  9 },
    { PIN_ID(PORT_F, 7),  11, 2, false, 13 },
    { PIN_ID(PORT_F, 8),  10, 1, false,  9 },
    { PIN_ID(PORT_F, 8),  11, 3, false, 13 },
    { PIN_ID(PORT_F, 9),  10, 2, false,  9 },
    { PIN_ID(PORT_F, 12), 12, 3, false, 13 },
    { PIN_ID(PORT_F, 13), 12, 4, false, 13 },
};

#define PWM_TIMER_MAX (12)

/* What each timer is doing, so that a second PWM on the same timer can be
 * given a channel that is actually free, and so that a frequency change can
 * rescale the channels it moves. Indexed by timer number minus one.
 *
 * The duty is kept here rather than in the PWM object because the object is on
 * the GC heap: a soft reset throws it away while the timer keeps running, and
 * nothing may follow a stale pointer to shut it down. */
typedef struct _machine_pwm_timer_t {
    uint8_t claimed;             /* bit per channel, 0-3 */
    uint8_t owner[4];            /* pin id holding each claimed channel */
    uint16_t duty_u16[4];        /* as requested, before rounding to a period */
} machine_pwm_timer_t;

static machine_pwm_timer_t machine_pwm_timers[PWM_TIMER_MAX];

typedef struct _machine_pwm_obj_t {
    mp_obj_base_t base;
    uint8_t pin;
    uint8_t timer;               /* 1-12; 0 once deinit()ed */
    uint8_t channel;             /* 1-4 */
    uint8_t af;
    bool negated;
    bool invert;
} machine_pwm_obj_t;

static TIM_TypeDef *machine_pwm_tim(uint8_t timer) {
    static TIM_TypeDef *const tims[PWM_TIMER_MAX] = {
        TIM1, TIM2, TIM3, TIM4, TIM5, TIM6,
        TIM7, TIM8, TIM9, TIM10, TIM11, TIM12,
    };
    return tims[timer - 1];
}

/* Where a timer's clock-enable and reset bits live. TIM1, TIM8 and TIM9-TIM12
 * hang off HB2 with the other high-speed peripherals; TIM2-TIM7 are on HB1.
 * Same split as SPI, and the opposite of I2C's. Returns true for HB2. */
static bool machine_pwm_rcc_bit(uint8_t timer, uint32_t *mask) {
    static const uint32_t hb1[] = {
        RCC_HB1Periph_TIM2, RCC_HB1Periph_TIM3, RCC_HB1Periph_TIM4,
        RCC_HB1Periph_TIM5, RCC_HB1Periph_TIM6, RCC_HB1Periph_TIM7,
    };
    static const uint32_t hb2[] = {
        RCC_HB2Periph_TIM9, RCC_HB2Periph_TIM10,
        RCC_HB2Periph_TIM11, RCC_HB2Periph_TIM12,
    };
    if (timer == 1) {
        *mask = RCC_HB2Periph_TIM1;
    } else if (timer == 8) {
        *mask = RCC_HB2Periph_TIM8;
    } else if (timer >= 9) {
        *mask = hb2[timer - 9];
    } else {
        *mask = hb1[timer - 2];
        return false;
    }
    return true;
}

static void machine_pwm_clock_enable(uint8_t timer, FunctionalState state) {
    uint32_t mask;
    if (machine_pwm_rcc_bit(timer, &mask)) {
        RCC_HB2PeriphClockCmd(mask, state);
    } else {
        RCC_HB1PeriphClockCmd(mask, state);
    }
}

/* Pulse the peripheral reset, so a timer starts from its documented state. */
static void machine_pwm_reset(uint8_t timer) {
    uint32_t mask;
    if (machine_pwm_rcc_bit(timer, &mask)) {
        RCC_HB2PeriphResetCmd(mask, ENABLE);
        RCC_HB2PeriphResetCmd(mask, DISABLE);
    } else {
        RCC_HB1PeriphResetCmd(mask, ENABLE);
        RCC_HB1PeriphResetCmd(mask, DISABLE);
    }
}

/* Only TIM1 and TIM8 have complementary outputs and a break circuit, and only
 * they gate their pins behind the master output enable. */
static bool machine_pwm_is_advanced(uint8_t timer) {
    return timer == 1 || timer == 8;
}

/* The timers count HCLK, not the 400 MHz core clock and not SystemCoreClock.
 * Ask RCC rather than assuming, exactly as mphalport.c does for SysTick: this
 * chip's core clock is four times its bus clock, and hard-coding the wrong one
 * would put every PWM frequency out by 4x. */
static uint32_t machine_pwm_source_hz(void) {
    RCC_ClocksTypeDef clocks;
    RCC_GetClocksFreq(&clocks);
    return clocks.HCLK_Frequency;
}

/* Split a frequency into prescaler and period.
 *
 * Prefers the smallest prescaler that keeps the period in range, because the
 * period is also the duty resolution: at 1 kHz the counter runs to 50000 and
 * duty_u16 is exact to about 1.3 parts in 65535, while a larger prescaler
 * would quantise it coarsely for no benefit.
 */
static bool machine_pwm_timing_for(uint32_t hz, mp_int_t freq,
    uint16_t *psc_out, uint32_t *period_out) {
    if (freq <= 0 || (uint32_t)freq > hz / 2) {
        return false;
    }
    uint64_t ticks = ((uint64_t)hz + freq / 2) / (uint64_t)freq;
    uint32_t psc = (uint32_t)((ticks + PWM_PERIOD_MAX - 1) / PWM_PERIOD_MAX);
    if (psc == 0) {
        psc = 1;
    }
    if (psc > 65536) {
        return false;
    }
    uint32_t period = (uint32_t)((ticks + psc / 2) / psc);
    if (period < 2) {
        period = 2;
    } else if (period > PWM_PERIOD_MAX) {
        period = PWM_PERIOD_MAX;
    }
    *psc_out = (uint16_t)(psc - 1);
    *period_out = period;
    return true;
}

static uint32_t machine_pwm_period(TIM_TypeDef *tim) {
    return (uint32_t)tim->ATRLR + 1;
}

/* duty_u16 is documented as the ratio duty_u16 / 65535, so the maximum really
 * does mean "on for the whole period". That needs a compare value above the
 * reload value, which is why the period is capped one below the counter's
 * range. */
static uint16_t machine_pwm_ccr_for(uint32_t period, uint16_t duty_u16) {
    if (duty_u16 >= 65535) {
        return (uint16_t)period;
    }
    return (uint16_t)(((uint32_t)duty_u16 * period + 32767u) / 65535u);
}

static uint16_t machine_pwm_duty_from_ccr(uint32_t period, uint32_t ccr) {
    if (ccr >= period) {
        return 65535;
    }
    return (uint16_t)((ccr * 65535u + period / 2) / period);
}

static void machine_pwm_set_ccr(TIM_TypeDef *tim, uint8_t channel, uint16_t ccr) {
    switch (channel) {
        case 1:
            tim->CH1CVR = ccr;
            break;
        case 2:
            tim->CH2CVR = ccr;
            break;
        case 3:
            tim->CH3CVR = ccr;
            break;
        default:
            tim->CH4CVR = ccr;
            break;
    }
}

static uint16_t machine_pwm_get_ccr(TIM_TypeDef *tim, uint8_t channel) {
    switch (channel) {
        case 1:
            return tim->CH1CVR;
        case 2:
            return tim->CH2CVR;
        case 3:
            return tim->CH3CVR;
        default:
            return tim->CH4CVR;
    }
}

static void machine_pwm_init_pin(uint8_t pin, uint8_t af) {
    machine_pin_clock_enable(pin);
    /* GPIO_PinAFConfig writes land in the AFIO block and are silently dropped
     * while its clock is off. */
    RCC_HB2PeriphClockCmd(RCC_HB2Periph_AFIO, ENABLE);

    GPIO_InitTypeDef init = {0};
    init.GPIO_Mode = GPIO_Mode_AF_PP;
    init.GPIO_Speed = GPIO_Speed_Very_High;
    init.GPIO_Pin = (uint16_t)(1u << MACHINE_PIN_NUM(pin));
    GPIO_Init(machine_pin_gpio(pin), &init);

    GPIO_PinAFConfig(machine_pin_gpio(pin), MACHINE_PIN_NUM(pin), af);
}

/* Hand the pin back as a floating input. Leaving it in alternate-function mode
 * with the timer stopped would park it at whatever level the last compare left
 * behind, which for a motor driver is the difference between "off" and "full
 * on". */
static void machine_pwm_release_pin(uint8_t pin) {
    machine_pin_clock_enable(pin);
    GPIO_InitTypeDef init = {0};
    init.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    init.GPIO_Speed = GPIO_Speed_Very_High;
    init.GPIO_Pin = (uint16_t)(1u << MACHINE_PIN_NUM(pin));
    GPIO_Init(machine_pin_gpio(pin), &init);
}

static void machine_pwm_config_channel(machine_pwm_obj_t *self, uint16_t ccr) {
    TIM_TypeDef *tim = machine_pwm_tim(self->timer);

    TIM_OCInitTypeDef oc;
    TIM_OCStructInit(&oc);
    oc.TIM_OCMode = TIM_OCMode_PWM1;
    oc.TIM_Pulse = ccr;
    if (self->negated) {
        oc.TIM_OutputState = TIM_OutputState_Disable;
        oc.TIM_OutputNState = TIM_OutputNState_Enable;
        /* Measured, not assumed: with the dead-time generator disabled this
         * silicon drives OCyN from OCyREF directly rather than from its
         * inverse, so CCyNP works exactly like CCyP and duty_u16 means the
         * same fraction-of-time-high on a CHyN pin as on a CHy one. Setting
         * the polarity the other way round -- which is what the half-bridge
         * reading of the reference manual suggests -- put a requested 25 % out
         * at 75 %, which is what test_pwm.py checks for. */
        oc.TIM_OCNPolarity = self->invert ? TIM_OCNPolarity_Low : TIM_OCNPolarity_High;
    } else {
        oc.TIM_OutputState = TIM_OutputState_Enable;
        oc.TIM_OCPolarity = self->invert ? TIM_OCPolarity_Low : TIM_OCPolarity_High;
    }

    switch (self->channel) {
        case 1:
            TIM_OC1Init(tim, &oc);
            TIM_OC1PreloadConfig(tim, TIM_OCPreload_Enable);
            break;
        case 2:
            TIM_OC2Init(tim, &oc);
            TIM_OC2PreloadConfig(tim, TIM_OCPreload_Enable);
            break;
        case 3:
            TIM_OC3Init(tim, &oc);
            TIM_OC3PreloadConfig(tim, TIM_OCPreload_Enable);
            break;
        default:
            TIM_OC4Init(tim, &oc);
            TIM_OC4PreloadConfig(tim, TIM_OCPreload_Enable);
            break;
    }
}

/* Push a new period into a timer and move every channel already running on it
 * to the compare value that keeps its requested duty. */
static void machine_pwm_apply_timing(uint8_t timer, uint16_t psc, uint32_t period) {
    TIM_TypeDef *tim = machine_pwm_tim(timer);
    machine_pwm_timer_t *state = &machine_pwm_timers[timer - 1];

    if (tim->PSC == psc && tim->ATRLR == (uint16_t)(period - 1)) {
        /* Already there. Returning early is not just an optimisation: the
         * update event below restarts the counter, so doing this on every
         * init() would jump the phase of channels that are already running
         * every time a second output joins their timer. */
        return;
    }

    tim->PSC = psc;
    tim->ATRLR = (uint16_t)(period - 1);
    for (uint8_t ch = 0; ch < 4; ch++) {
        if (state->claimed & (1u << ch)) {
            machine_pwm_set_ccr(tim, ch + 1, machine_pwm_ccr_for(period, state->duty_u16[ch]));
        }
    }
    /* The prescaler is buffered; without an update event the old one keeps
     * running until the counter next wraps, which at a low frequency is
     * seconds away. */
    TIM_GenerateEvent(tim, TIM_EventSource_Update);
}

static mp_int_t machine_pwm_freq_of(uint8_t timer) {
    TIM_TypeDef *tim = machine_pwm_tim(timer);
    uint32_t divisor = ((uint32_t)tim->PSC + 1) * machine_pwm_period(tim);
    uint32_t hz = machine_pwm_source_hz();
    return (mp_int_t)((hz + divisor / 2) / divisor);
}

/* Pick a timer and channel for a pin.
 *
 * A pin reaches up to four different channels, and which one is chosen decides
 * what else the PWM will drag around with it, because the frequency belongs to
 * the timer. So the order of preference is: a timer already running at the
 * frequency being asked for (joining it costs nothing), then a timer nobody is
 * using (so a later freq() call disturbs nothing), then any free channel.
 *
 * Re-creating a PWM on a pin that already has one is not a conflict -- it is
 * how init() gets a fresh configuration -- so a channel the same pin already
 * owns counts as free.
 */
static const machine_pwm_af_t *machine_pwm_choose(uint8_t pin, mp_int_t timer_req, mp_int_t freq) {
    const machine_pwm_af_t *unused_timer = NULL;
    const machine_pwm_af_t *free_channel = NULL;
    bool pin_has_options = false;

    for (size_t i = 0; i < MP_ARRAY_SIZE(machine_pwm_af_options); i++) {
        const machine_pwm_af_t *opt = &machine_pwm_af_options[i];
        if (opt->pin != pin) {
            continue;
        }
        pin_has_options = true;
        if (timer_req >= 0 && opt->timer != timer_req) {
            continue;
        }
        machine_pwm_timer_t *state = &machine_pwm_timers[opt->timer - 1];
        uint8_t bit = 1u << (opt->channel - 1);
        if ((state->claimed & bit) && state->owner[opt->channel - 1] != pin) {
            continue;
        }
        if (state->claimed == 0) {
            if (unused_timer == NULL) {
                unused_timer = opt;
            }
        } else if (freq > 0 && machine_pwm_freq_of(opt->timer) == freq) {
            return opt;
        }
        if (free_channel == NULL) {
            free_channel = opt;
        }
    }

    if (free_channel == NULL) {
        if (!pin_has_options) {
            mp_raise_ValueError(MP_ERROR_TEXT("pin has no PWM output"));
        }
        if (timer_req >= 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("pin cannot reach that timer, or its channel is in use"));
        }
        mp_raise_ValueError(MP_ERROR_TEXT("every timer channel this pin can reach is in use"));
    }
    return unused_timer != NULL ? unused_timer : free_channel;
}

static void machine_pwm_release(machine_pwm_obj_t *self) {
    if (self->timer == 0) {
        return;
    }
    TIM_TypeDef *tim = machine_pwm_tim(self->timer);
    machine_pwm_timer_t *state = &machine_pwm_timers[self->timer - 1];

    if (self->negated) {
        TIM_CCxNCmd(tim, (uint16_t)((self->channel - 1) * 4), TIM_CCxN_Disable);
    } else {
        TIM_CCxCmd(tim, (uint16_t)((self->channel - 1) * 4), TIM_CCx_Disable);
    }
    machine_pwm_release_pin(self->pin);

    state->claimed &= (uint8_t) ~(1u << (self->channel - 1));
    if (state->claimed == 0) {
        /* Nothing left on this timer: stop it and take its clock away rather
         * than leaving a counter running for no one. */
        if (machine_pwm_is_advanced(self->timer)) {
            TIM_CtrlPWMOutputs(tim, DISABLE);
        }
        TIM_Cmd(tim, DISABLE);
        machine_pwm_clock_enable(self->timer, DISABLE);
    }
    self->timer = 0;
}

void machine_pwm_deinit_all(void) {
    for (uint8_t timer = 1; timer <= PWM_TIMER_MAX; timer++) {
        machine_pwm_timer_t *state = &machine_pwm_timers[timer - 1];
        if (state->claimed == 0) {
            continue;
        }
        TIM_TypeDef *tim = machine_pwm_tim(timer);
        for (uint8_t ch = 0; ch < 4; ch++) {
            if (state->claimed & (1u << ch)) {
                machine_pwm_release_pin(state->owner[ch]);
            }
        }
        if (machine_pwm_is_advanced(timer)) {
            TIM_CtrlPWMOutputs(tim, DISABLE);
        }
        TIM_Cmd(tim, DISABLE);
        machine_pwm_clock_enable(timer, DISABLE);
        state->claimed = 0;
    }
}

/* --- the interface extmod/machine_pwm.c calls --- */

static void mp_machine_pwm_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    machine_pwm_obj_t *self = MP_OBJ_TO_PTR(self_in);
    static const char ports[] = "ABCDEF";
    if (self->timer == 0) {
        mp_printf(print, "PWM(P%c%u)", ports[MACHINE_PIN_PORT(self->pin)],
            (unsigned)MACHINE_PIN_NUM(self->pin));
        return;
    }
    TIM_TypeDef *tim = machine_pwm_tim(self->timer);
    uint32_t period = machine_pwm_period(tim);
    mp_printf(print, "PWM(P%c%u, timer=%u, channel=%u%s, freq=%u, duty_u16=%u%s)",
        ports[MACHINE_PIN_PORT(self->pin)], (unsigned)MACHINE_PIN_NUM(self->pin),
        (unsigned)self->timer, (unsigned)self->channel, self->negated ? "N" : "",
        (unsigned)machine_pwm_freq_of(self->timer),
        (unsigned)machine_pwm_duty_from_ccr(period, machine_pwm_get_ccr(tim, self->channel)),
        self->invert ? ", invert=True" : "");
}

enum { ARG_freq, ARG_duty_u16, ARG_duty_ns, ARG_invert, ARG_timer };
static const mp_arg_t machine_pwm_allowed_args[] = {
    { MP_QSTR_freq,     MP_ARG_KW_ONLY | MP_ARG_INT,  {.u_int = -1} },
    { MP_QSTR_duty_u16, MP_ARG_KW_ONLY | MP_ARG_INT,  {.u_int = -1} },
    { MP_QSTR_duty_ns,  MP_ARG_KW_ONLY | MP_ARG_INT,  {.u_int = -1} },
    { MP_QSTR_invert,   MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
    { MP_QSTR_timer,    MP_ARG_KW_ONLY | MP_ARG_INT,  {.u_int = -1} },
};

/* Compare ticks for a pulse width, given the prescaler that is about to be
 * programmed -- so this works before the timer holds the new timing. */
static uint32_t machine_pwm_ticks_for_ns(uint16_t psc, mp_int_t duty_ns) {
    return (uint32_t)((uint64_t)duty_ns * machine_pwm_source_hz()
        / ((uint64_t)1000000000u * ((uint32_t)psc + 1)));
}

static void mp_machine_pwm_init_helper(machine_pwm_obj_t *self,
    size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    mp_arg_val_t args[MP_ARRAY_SIZE(machine_pwm_allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args,
        MP_ARRAY_SIZE(machine_pwm_allowed_args), machine_pwm_allowed_args, args);

    if (args[ARG_duty_u16].u_int >= 0 && args[ARG_duty_ns].u_int >= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("specify only one of duty_u16 and duty_ns"));
    }

    mp_int_t freq = args[ARG_freq].u_int;
    mp_int_t timer_req = args[ARG_timer].u_int;
    if (timer_req >= 0 && (timer_req < 1 || timer_req > PWM_TIMER_MAX
                           || timer_req == 6 || timer_req == 7)) {
        /* TIM6 and TIM7 exist but have no output pins at all. */
        mp_raise_ValueError(MP_ERROR_TEXT("no such PWM timer"));
    }

    /* A timer already claimed by this object stays claimed while we choose,
     * which is what makes re-init on the same pin land back on the same
     * channel instead of hopping to another timer. */
    const machine_pwm_af_t *opt = machine_pwm_choose(self->pin, timer_req, freq);

    if (self->timer != 0 && (self->timer != opt->timer || self->channel != opt->channel)) {
        machine_pwm_release(self);
    }

    self->timer = opt->timer;
    self->channel = opt->channel;
    self->negated = opt->negated;
    self->af = opt->af;
    self->invert = args[ARG_invert].u_bool;

    TIM_TypeDef *tim = machine_pwm_tim(self->timer);
    machine_pwm_timer_t *state = &machine_pwm_timers[self->timer - 1];
    bool first_on_timer = state->claimed == 0;

    if (first_on_timer) {
        machine_pwm_clock_enable(self->timer, ENABLE);
        /* Reset before configuring. Taking a timer's clock away in deinit()
         * does not clear its registers, so without this the channels a
         * previous PWM had enabled come back the moment the clock returns --
         * and drive pins this object never asked for. */
        machine_pwm_reset(self->timer);
        TIM_TimeBaseInitTypeDef base;
        TIM_TimeBaseStructInit(&base);
        base.TIM_CounterMode = TIM_CounterMode_Up;
        base.TIM_ClockDivision = TIM_CKD_DIV1;
        base.TIM_Prescaler = 0;
        base.TIM_Period = PWM_PERIOD_MAX - 1;
        TIM_TimeBaseInit(tim, &base);
        TIM_ARRPreloadConfig(tim, ENABLE);
    }

    if (freq < 0) {
        freq = first_on_timer ? PWM_DEFAULT_FREQ : machine_pwm_freq_of(self->timer);
    }
    uint16_t psc;
    uint32_t period;
    if (!machine_pwm_timing_for(machine_pwm_source_hz(), freq, &psc, &period)) {
        mp_raise_ValueError(MP_ERROR_TEXT("frequency out of range"));
    }

    /* Settle the duty before touching the hardware, not after.
     *
     * The compare registers are preloaded, so a value written after the timer
     * is running does not reach the output until the next update event -- a
     * whole period, which at 1 Hz is a second of the pin sitting at the wrong
     * level. Folding the requested duty in here means the update event that
     * loads the new period loads the duty with it, and PWM(pin, freq=1,
     * duty_u16=32768) is at 50 % from its first cycle. */
    uint8_t ch_index = self->channel - 1;
    uint32_t ccr;
    if (args[ARG_duty_u16].u_int >= 0) {
        if (args[ARG_duty_u16].u_int > 65535) {
            mp_raise_ValueError(MP_ERROR_TEXT("duty_u16 must be 0 to 65535"));
        }
        state->duty_u16[ch_index] = (uint16_t)args[ARG_duty_u16].u_int;
        ccr = machine_pwm_ccr_for(period, state->duty_u16[ch_index]);
    } else if (args[ARG_duty_ns].u_int >= 0) {
        ccr = machine_pwm_ticks_for_ns(psc, args[ARG_duty_ns].u_int);
        if (ccr > period) {
            mp_raise_ValueError(MP_ERROR_TEXT("duty_ns longer than the period"));
        }
        state->duty_u16[ch_index] = machine_pwm_duty_from_ccr(period, ccr);
    } else {
        /* Nothing asked for: keep what this channel already had, and start a
         * brand new one at zero rather than at whatever the reset value of the
         * compare register happens to be. */
        if (!(state->claimed & (1u << ch_index))) {
            state->duty_u16[ch_index] = 0;
        }
        ccr = machine_pwm_ccr_for(period, state->duty_u16[ch_index]);
    }
    /* Claim before applying the timing, so the rescale loop sees this channel. */
    state->claimed |= (uint8_t)(1u << ch_index);
    state->owner[ch_index] = self->pin;

    machine_pwm_config_channel(self, (uint16_t)ccr);
    machine_pwm_apply_timing(self->timer, psc, period);

    if (machine_pwm_is_advanced(self->timer)) {
        /* Advanced timers keep their pins disconnected until the master output
         * enable is set, so without this TIM1 and TIM8 look configured and
         * output nothing. */
        TIM_CtrlPWMOutputs(tim, ENABLE);
    }
    TIM_Cmd(tim, ENABLE);

    machine_pwm_init_pin(self->pin, self->af);
}

static mp_obj_t mp_machine_pwm_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, MP_OBJ_FUN_ARGS_MAX, true);

    const machine_pin_obj_t *pin = machine_pin_get(args[0]);

    machine_pwm_obj_t *self = mp_obj_malloc(machine_pwm_obj_t, type);
    self->pin = pin->id;
    self->timer = 0;

    mp_map_t kw_args;
    mp_map_init_fixed_table(&kw_args, n_kw, args + n_args);
    mp_machine_pwm_init_helper(self, n_args - 1, args + 1, &kw_args);
    return MP_OBJ_FROM_PTR(self);
}

static void mp_machine_pwm_deinit(machine_pwm_obj_t *self) {
    machine_pwm_release(self);
}

/* Raise on a PWM whose deinit() has already run, rather than reading a timer
 * whose clock is off -- which returns zeros and would report freq 0. */
static TIM_TypeDef *machine_pwm_active(machine_pwm_obj_t *self) {
    if (self->timer == 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("PWM is deinitialised"));
    }
    return machine_pwm_tim(self->timer);
}

static mp_obj_t mp_machine_pwm_freq_get(machine_pwm_obj_t *self) {
    machine_pwm_active(self);
    return MP_OBJ_NEW_SMALL_INT(machine_pwm_freq_of(self->timer));
}

static void mp_machine_pwm_freq_set(machine_pwm_obj_t *self, mp_int_t freq) {
    machine_pwm_active(self);
    uint16_t psc;
    uint32_t period;
    if (!machine_pwm_timing_for(machine_pwm_source_hz(), freq, &psc, &period)) {
        mp_raise_ValueError(MP_ERROR_TEXT("frequency out of range"));
    }
    machine_pwm_apply_timing(self->timer, psc, period);
}

static mp_obj_t mp_machine_pwm_duty_get_u16(machine_pwm_obj_t *self) {
    TIM_TypeDef *tim = machine_pwm_active(self);
    return MP_OBJ_NEW_SMALL_INT(machine_pwm_duty_from_ccr(
        machine_pwm_period(tim), machine_pwm_get_ccr(tim, self->channel)));
}

static void mp_machine_pwm_duty_set_u16(machine_pwm_obj_t *self, mp_int_t duty_u16) {
    TIM_TypeDef *tim = machine_pwm_active(self);
    if (duty_u16 < 0 || duty_u16 > 65535) {
        mp_raise_ValueError(MP_ERROR_TEXT("duty_u16 must be 0 to 65535"));
    }
    uint32_t period = machine_pwm_period(tim);
    machine_pwm_timers[self->timer - 1].duty_u16[self->channel - 1] = (uint16_t)duty_u16;
    machine_pwm_set_ccr(tim, self->channel, machine_pwm_ccr_for(period, (uint16_t)duty_u16));
}

static mp_obj_t mp_machine_pwm_duty_get_ns(machine_pwm_obj_t *self) {
    TIM_TypeDef *tim = machine_pwm_active(self);
    uint64_t tick_ns = (uint64_t)1000000000u * ((uint32_t)tim->PSC + 1) / machine_pwm_source_hz();
    return MP_OBJ_NEW_SMALL_INT((mp_int_t)(machine_pwm_get_ccr(tim, self->channel) * tick_ns));
}

static void mp_machine_pwm_duty_set_ns(machine_pwm_obj_t *self, mp_int_t duty_ns) {
    TIM_TypeDef *tim = machine_pwm_active(self);
    if (duty_ns < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("duty_ns must not be negative"));
    }
    uint32_t period = machine_pwm_period(tim);
    uint64_t ticks = (uint64_t)duty_ns * machine_pwm_source_hz()
        / ((uint64_t)1000000000u * ((uint32_t)tim->PSC + 1));
    if (ticks > period) {
        mp_raise_ValueError(MP_ERROR_TEXT("duty_ns longer than the period"));
    }
    /* Keep the stored duty in step, so a later freq() change scales this pulse
     * with the period rather than reverting to whatever was set before. */
    machine_pwm_timers[self->timer - 1].duty_u16[self->channel - 1] =
        machine_pwm_duty_from_ccr(period, (uint32_t)ticks);
    machine_pwm_set_ccr(tim, self->channel, (uint16_t)ticks);
}
