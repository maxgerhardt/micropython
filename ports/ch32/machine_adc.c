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

#include "machine_adc.h"
#include "machine_pin.h"

#define ADC_CHANNEL_NONE (0xff)

/* No port-specific class constants. extmod/machine_adc.c pastes this into the
 * type's locals dict and provides no default, so it has to be defined even
 * when empty. */
#define MICROPY_PY_MACHINE_ADC_CLASS_CONSTANTS

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
} machine_adc_obj_t;

static bool adc_initialised;

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

    ADC_InitTypeDef init = {0};
    init.ADC_Mode = ADC_Mode_Independent;
    init.ADC_ScanConvMode = DISABLE;
    init.ADC_ContinuousConvMode = DISABLE;
    init.ADC_ExternalTrigConv = ADC_ExternalTrigConv_None;
    init.ADC_DataAlign = ADC_DataAlign_Right;
    init.ADC_NbrOfChannel = 1;
    ADC_Init(ADC1, &init);
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
