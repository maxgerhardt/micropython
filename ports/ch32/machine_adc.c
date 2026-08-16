/* machine.ADC for the CH32H417.
 *
 * Included by extmod/machine_adc.c through MICROPY_PY_MACHINE_ADC_INCLUDEFILE,
 * so the mp_machine_adc_* functions below must keep the static linkage that
 * file declares for them.
 *
 * ADC1 only, single conversion on demand. The peripheral is the STM32F1-style
 * one: configure a regular channel of rank 1, kick a software conversion, wait
 * for EOC, read DATAR.
 */
#include "ch32h417.h"

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/runtime.h"

#include "irq.h"
#include "machine_adc.h"
#include "machine_pin.h"
#include "machine_timer.h"

#define ADC_CHANNEL_NONE (0xff)

/* read_timed(): TIM3 update -> TRGO -> ADC1 external trigger, and each result
 * moved out of RDATAR by DMA. The CPU does nothing at all during a capture.
 *
 * TIM3 rather than TIM6, which machine.AudioOut holds, and which is not one of
 * the ADC's trigger sources on this part anyway -- the regular group can be
 * started from TIM1 CC1/CC2/CC3, TIM2 CC2, TIM3 TRGO, TIM4 CC4 or TIM8 TRGO,
 * and TRGO is the one that needs no output compare channel set up. */
#define ADC_TIMER_ID (3)
#define ADC_TIMER (TIM3)

/* Reference manual table 10-2: ADC1 is request 120, ADC2 is 121. DMA1
 * channels 1-6 are AudioOut, SPI, I2S and I2C; 7 and 8 are free. */
#define ADC_DMA_REQ_ADC1 (120)
#define ADC_DMA_CHANNEL (DMA1_Channel7)
#define ADC_DMA_MUX_CHANNEL (DMA_MuxChannel7)
#define ADC_DMA_IRQN (DMA1_Channel7_IRQn)
#define ADC_DMA_FLAGS (DMA1_IT_GL7 | DMA1_IT_TC7 | DMA1_IT_HT7)

/* Sample time per SMP setting, in half ADC clock cycles, from the reference
 * manual's SMPx table: 1.5, 7.5, 13.5, 28.5, 41.5, 55.5, 71.5 and 239.5
 * cycles. The 12-bit conversion adds 12.5 cycles of its own on top of
 * whichever is chosen. */
static const uint16_t adc_sample_halfcycles[8] = { 3, 15, 27, 57, 83, 111, 143, 479 };
#define ADC_CONVERT_HALFCYCLES (25)

/* ADCCLK, which machine_adc_init_hardware() sets to HCLK/8. */
#define ADC_CLOCK_DIVISOR (8)

/* irq() is spliced in through the class-constants macro. That macro is
 * pasted straight into extmod's locals dict table, so it can carry a method
 * as easily as a constant, and it is the only place extmod offers a port to
 * add anything -- there is no attr hook on this type. */
#define MICROPY_PY_MACHINE_ADC_CLASS_CONSTANTS \
    { MP_ROM_QSTR(MP_QSTR_irq), MP_ROM_PTR(&machine_adc_irq_obj) },

/* ADC input number for each pin that has one, indexed by pin id. Taken from
 * the datasheet's pin table: PA0-PA7 are IN0-IN7, PB0/PB1 are IN8/IN9, and
 * PC0-PC5 are IN10-IN15. Pins without an analog function hold NONE. */
uint8_t machine_adc_channel_for_pin(uint8_t pin_id) {
    uint8_t port = MACHINE_PIN_PORT(pin_id);
    uint8_t num = MACHINE_PIN_NUM(pin_id);
    if (port == 0 && num <= 7) {
        return num;                 /* PA0-PA7  -> IN0-IN7  */
    }
    if (port == 1 && num <= 1) {
        return (uint8_t)(8 + num);  /* PB0-PB1  -> IN8-IN9  */
    }
    if (port == 2 && num <= 5) {
        return (uint8_t)(10 + num); /* PC0-PC5  -> IN10-IN15 */
    }
    return ADC_CHANNEL_NONE;
}

typedef struct _machine_adc_obj_t {
    mp_obj_base_t base;
    uint8_t channel;
    uint8_t pin_id;
    /* Set by irq(). Also decides whether read_timed() waits for the capture
     * or starts it and returns. */
    mp_obj_t handler;
} machine_adc_obj_t;

static bool adc_initialised;

/* A capture in flight, or NULL. One ADC and one DMA channel, so there can only
 * ever be one, and it does not belong to any particular ADC object: the
 * hardware is shared and the pin is only the channel it happens to be reading.
 */
static volatile bool adc_timed_busy;

/* Everything the regular group's configuration needs, in one place, because
 * read_timed() has to switch the trigger to TIM3 and switch it back again
 * afterwards -- read_u16() on any pin must keep working either side of a
 * capture. */
static void adc_configure(uint32_t trigger) {
    ADC_InitTypeDef init = {0};
    init.ADC_Mode = ADC_Mode_Independent;
    init.ADC_ScanConvMode = DISABLE;
    init.ADC_ContinuousConvMode = DISABLE;
    init.ADC_ExternalTrigConv = trigger;
    init.ADC_DataAlign = ADC_DataAlign_Right;
    init.ADC_NbrOfChannel = 1;
    ADC_Init(ADC1, &init);
}

void machine_adc_init_hardware(void) {
    if (adc_initialised) {
        return;
    }

    /* ADCCLK = HCLK / (PPRE2 * ADCPRE). HCLK is 100 MHz here and the converter
     * is specified well below that, so take the largest divider the ADCPRE
     * field offers. A conversion is only a few microseconds either way, and
     * accuracy matters more than speed for a general-purpose ADC. */
    RCC_ADCCLKConfig(RCC_ADCCLKSource_HCLK);
    RCC->CFGR0 = (RCC->CFGR0 & ~(uint32_t)RCC_ADCPRE)
        | ((uint32_t)RCC_HCLK_ADCPRE_DIV8 << 14);

    RCC_HB2PeriphClockCmd(RCC_HB2Periph_ADC1, ENABLE);

    adc_configure(ADC_ExternalTrigConv_None);
    ADC_Cmd(ADC1, ENABLE);

    /* Calibrate once, after the converter is powered. Skipping this costs
     * tens of LSBs of offset. */
    ADC_ResetCalibration(ADC1);
    while (ADC_GetResetCalibrationStatus(ADC1)) {
    }
    ADC_StartCalibration(ADC1);
    while (ADC_GetCalibrationStatus(ADC1)) {
    }

    adc_initialised = true;
}

static void mp_machine_adc_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    machine_adc_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "ADC(Pin(P%c%u), channel=%u)",
        'A' + MACHINE_PIN_PORT(self->pin_id), MACHINE_PIN_NUM(self->pin_id), self->channel);
}

static mp_obj_t mp_machine_adc_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, 1, false);

    const machine_pin_obj_t *pin = machine_pin_get(args[0]);
    uint8_t channel = machine_adc_channel_for_pin(pin->id);
    if (channel == ADC_CHANNEL_NONE) {
        mp_raise_ValueError(MP_ERROR_TEXT("pin has no ADC channel"));
    }

    machine_adc_init_hardware();

    /* Analog mode disconnects the digital input buffer, which would otherwise
     * both load the signal and burn current at mid-rail. */
    machine_pin_clock_enable(pin->id);
    GPIO_InitTypeDef gpio = {0};
    gpio.GPIO_Pin = MACHINE_PIN_MASK(pin->id);
    gpio.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(machine_pin_gpio(pin->id), &gpio);

    machine_adc_obj_t *self = mp_obj_malloc(machine_adc_obj_t, type);
    self->channel = channel;
    self->pin_id = pin->id;
    self->handler = MP_OBJ_NULL;
    return MP_OBJ_FROM_PTR(self);
}

static mp_int_t adc_read_raw(machine_adc_obj_t *self) {
    /* Longest sample time available: the source impedance of whatever is
     * attached is unknown, and a short window on a high-impedance source
     * reads low because the sampling capacitor never finishes charging. */
    ADC_RegularChannelConfig(ADC1, self->channel, 1, ADC_SampleTime_CyclesMode7);
    ADC_ClearFlag(ADC1, ADC_FLAG_EOC);
    ADC_SoftwareStartConvCmd(ADC1, ENABLE);

    uint64_t deadline = mp_hal_ticks_us() + 10000;
    while (!ADC_GetFlagStatus(ADC1, ADC_FLAG_EOC)) {
        if (mp_hal_ticks_us() > deadline) {
            mp_raise_OSError(MP_ETIMEDOUT);
        }
    }
    return ADC_GetConversionValue(ADC1);
}

static mp_int_t mp_machine_adc_read_u16(machine_adc_obj_t *self) {
    /* The converter is 12-bit; MicroPython's read_u16() is defined as full
     * scale 0-65535, so replicate the top bits rather than shifting in zeros,
     * which would make full scale read 65520 instead of 65535. */
    uint32_t raw = adc_read_raw(self) & 0xfff;
    return (mp_int_t)((raw << 4) | (raw >> 8));
}

static mp_int_t mp_machine_adc_read_uv(machine_adc_obj_t *self) {
    /* Referenced to VREFP, which is tied to the 3.3 V rail on this board. */
    return (mp_int_t)((uint64_t)adc_read_raw(self) * 3300000u / 4095u);
}

/* --- read_timed(): a block of samples at a fixed rate --------------------- */

/* Put the converter back the way single conversions expect to find it, and
 * give the timer back. Called from both the blocking path and the interrupt,
 * so it touches nothing that needs the heap. */
static void adc_timed_stop(void) {
    TIM_Cmd(ADC_TIMER, DISABLE);
    ADC_ExternalTrigConvCmd(ADC1, DISABLE);
    ADC_DMACmd(ADC1, DISABLE);
    DMA_ITConfig(ADC_DMA_CHANNEL, DMA_IT_TC, DISABLE);
    DMA_Cmd(ADC_DMA_CHANNEL, DISABLE);
    /* Back to the software trigger. Without this a later read_u16() on any
     * pin would wait for a TIM3 that is no longer running, and time out. */
    adc_configure(ADC_ExternalTrigConv_None);
    MP_STATE_PORT(machine_adc_timed_self) = MP_OBJ_NULL;
    MP_STATE_PORT(machine_adc_timed_buf) = MP_OBJ_NULL;
    adc_timed_busy = false;
    ch32_timer_release(ADC_TIMER_ID, CH32_TIMER_ADC);
}

void CH32_IRQ_HANDLER(DMA1_Channel7_IRQHandler);
void DMA1_Channel7_IRQHandler(void) {
    DMA1->INTFCR = ADC_DMA_FLAGS;
    mp_obj_t self_in = MP_STATE_PORT(machine_adc_timed_self);
    adc_timed_stop();
    if (self_in != MP_OBJ_NULL) {
        machine_adc_obj_t *self = MP_OBJ_TO_PTR(self_in);
        if (self->handler != MP_OBJ_NULL) {
            mp_sched_schedule(self->handler, self_in);
        }
    }
}

/* Fill buf with samples taken at freq Hz.
 *
 * A timer triggers the conversions and DMA carries the results away, so the
 * interval between samples is hardware-exact and the CPU does nothing at all
 * while it happens. That is the difference between this and a Python loop
 * around read_u16(): the loop's spacing is whatever the interpreter managed,
 * which is neither known nor even.
 *
 * Samples are raw 12-bit values, 0 to 4095, not the 0-65535 that read_u16()
 * scales to -- the DMA moves what the converter produced and nothing gets a
 * chance to rescale it. buf must therefore hold 16-bit elements: array("H").
 */
static void mp_machine_adc_read_timed(machine_adc_obj_t *self, mp_obj_t buf_in, mp_int_t freq) {
    if (adc_timed_busy) {
        mp_raise_OSError(MP_EBUSY);
    }

    mp_buffer_info_t buf;
    mp_get_buffer_raise(buf_in, &buf, MP_BUFFER_WRITE);
    /* A bytearray is refused rather than filled with the low 8 bits of a
     * 12-bit result, which is what a byte-wide DMA would silently deliver. */
    if (buf.typecode != 'H' && buf.typecode != 'h') {
        mp_raise_ValueError(MP_ERROR_TEXT("buffer must be array('H'): samples are 12-bit"));
    }
    size_t count = buf.len / sizeof(uint16_t);
    if (count == 0 || count > 65535) {
        mp_raise_ValueError(MP_ERROR_TEXT("buffer must hold 1 to 65535 samples"));
    }
    if (freq < 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("freq must be positive"));
    }

    RCC_ClocksTypeDef clocks;
    RCC_GetClocksFreq(&clocks);
    uint32_t hclk = clocks.HCLK_Frequency;
    uint32_t adcclk = hclk / ADC_CLOCK_DIVISOR;

    /* The longest sample time that still fits inside one trigger period.
     *
     * Sample time is how long the converter's capacitor is connected to the
     * pin, so more of it is strictly better on a high-impedance source -- a
     * short window reads low because the capacitor never finishes charging.
     * There is no reason to take less than the rate allows, and picking it
     * here rather than fixing it means a slow capture gets the accuracy a
     * fast one cannot have. */
    uint32_t period_halves = (uint32_t)((2ull * adcclk) / (uint32_t)freq);
    int smp = -1;
    for (int i = MP_ARRAY_SIZE(adc_sample_halfcycles) - 1; i >= 0; i--) {
        if ((uint32_t)adc_sample_halfcycles[i] + ADC_CONVERT_HALFCYCLES <= period_halves) {
            smp = i;
            break;
        }
    }
    if (smp < 0) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("freq too high; the converter tops out near %u Hz"),
            (unsigned int)(2u * adcclk / (adc_sample_halfcycles[0] + ADC_CONVERT_HALFCYCLES)));
    }

    uint8_t held = ch32_timer_owner(ADC_TIMER_ID);
    if (held != CH32_TIMER_FREE && held != CH32_TIMER_ADC) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("timer %d is held by %s"),
            ADC_TIMER_ID, ch32_timer_owner_name(held));
    }
    ch32_timer_claim(ADC_TIMER_ID, CH32_TIMER_ADC);

    /* TIM3 counts at HCLK, like every timer on this part. */
    RCC_HB1PeriphClockCmd(RCC_HB1Periph_TIM3, ENABLE);
    TIM_DeInit(ADC_TIMER);
    uint32_t psc = 0;
    while ((hclk / (psc + 1)) / (uint32_t)freq > 0xFFFF) {
        psc++;
    }
    uint32_t period = (hclk / (psc + 1) + (uint32_t)freq / 2) / (uint32_t)freq;
    if (period < 2) {
        period = 2;
    }
    TIM_TimeBaseInitTypeDef tb = {0};
    tb.TIM_Prescaler = (uint16_t)psc;
    tb.TIM_CounterMode = TIM_CounterMode_Up;
    tb.TIM_Period = (uint16_t)(period - 1);
    tb.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseInit(ADC_TIMER, &tb);
    TIM_SelectOutputTrigger(ADC_TIMER, TIM_TRGOSource_Update);

    RCC_HBPeriphClockCmd(RCC_HBPeriph_DMA1, ENABLE);
    DMA_DeInit(ADC_DMA_CHANNEL);
    DMA_InitTypeDef dma = {0};
    dma.DMA_PeripheralBaseAddr = (uint32_t)&ADC1->RDATAR;
    dma.DMA_Memory0BaseAddr = (uint32_t)buf.buf;
    dma.DMA_DIR = DMA_DIR_PeripheralSRC;
    dma.DMA_BufferSize = (uint16_t)count;
    dma.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    dma.DMA_MemoryInc = DMA_MemoryInc_Enable;
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
    dma.DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;
    /* Normal, not circular: a capture is a fixed number of samples and
     * stopping at the end is what makes the buffer safe to read. */
    dma.DMA_Mode = DMA_Mode_Normal;
    dma.DMA_Priority = DMA_Priority_High;
    dma.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(ADC_DMA_CHANNEL, &dma);
    DMA_MuxChannelConfig(ADC_DMA_MUX_CHANNEL, ADC_DMA_REQ_ADC1);

    /* Trigger first, channel second: ADC_Init() rewrites the sequence length,
     * so configuring the channel before it would be undone. */
    adc_configure(ADC_ExternalTrigConv_T3_TRGO);
    ADC_RegularChannelConfig(ADC1, self->channel, 1, (uint8_t)smp);
    ADC_DMACmd(ADC1, ENABLE);
    ADC_ExternalTrigConvCmd(ADC1, ENABLE);

    DMA1->INTFCR = ADC_DMA_FLAGS;
    adc_timed_busy = true;
    MP_STATE_PORT(machine_adc_timed_self) = MP_OBJ_FROM_PTR(self);
    /* The DMA is writing into this buffer, so it has to stay alive even if
     * the caller drops its own reference the moment read_timed() returns --
     * which a non-blocking caller very reasonably might. */
    MP_STATE_PORT(machine_adc_timed_buf) = buf_in;

    if (self->handler != MP_OBJ_NULL) {
        DMA_ITConfig(ADC_DMA_CHANNEL, DMA_IT_TC, ENABLE);
        NVIC_EnableIRQ(ADC_DMA_IRQN);
    }
    DMA_Cmd(ADC_DMA_CHANNEL, ENABLE);
    TIM_Cmd(ADC_TIMER, ENABLE);

    if (self->handler != MP_OBJ_NULL) {
        return;         /* busy() and the callback report the end */
    }

    /* Blocking: the capture takes count/freq seconds by construction, so the
     * bound is that with room to spare rather than an arbitrary constant. */
    uint64_t expected_us = (uint64_t)count * 1000000ull / (uint32_t)freq;
    uint64_t deadline = mp_hal_ticks_us() + expected_us * 2 + 100000ull;
    while (DMA_GetCurrDataCounter(ADC_DMA_CHANNEL) != 0) {
        if (mp_hal_ticks_us() > deadline) {
            adc_timed_stop();
            mp_raise_OSError(MP_ETIMEDOUT);
        }
        mp_event_handle_nowait();
    }
    adc_timed_stop();
}

/* True while a read_timed() started with a callback set is still running.
 * Always False after a blocking one, which cannot return until it is done. */
static mp_obj_t mp_machine_adc_busy(machine_adc_obj_t *self) {
    (void)self;
    return mp_obj_new_bool(adc_timed_busy);
}

/* Set a callback and read_timed() starts the capture and returns; pass None
 * and it waits. The same call, and the same meaning, as irq() on machine.I2S,
 * machine.SPI, machine.I2C and machine.AudioOut here.
 *
 * The handler is called with the ADC object, from the scheduler rather than
 * from the interrupt, so it may allocate and raise like any other Python
 * code. */
static mp_obj_t machine_adc_irq(mp_obj_t self_in, mp_obj_t handler) {
    machine_adc_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (handler != mp_const_none && !mp_obj_is_callable(handler)) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid callback"));
    }
    self->handler = (handler == mp_const_none) ? MP_OBJ_NULL : handler;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(machine_adc_irq_obj, machine_adc_irq);

MP_REGISTER_ROOT_POINTER(mp_obj_t machine_adc_timed_self);
MP_REGISTER_ROOT_POINTER(mp_obj_t machine_adc_timed_buf);
