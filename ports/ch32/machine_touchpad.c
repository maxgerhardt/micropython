/* machine.TouchPad for the CH32H417, on the TKEY peripheral.
 *
 * TKEY is not a separate block: `TKey1` and `ADC1` are the same registers, and
 * two bits in CTLR1 turn the converter into a capacitance meter. A measurement
 * charges the pad for a fixed time, discharges it through the converter, and
 * reports how far it got -- so a finger, which adds capacitance, moves the
 * reading. That is the same shape as the ESP32's touch peripheral and the same
 * API applies:
 *
 *     t = machine.TouchPad(machine.Pin("PC4"))
 *     t.read()
 *
 * The vendor ships a precompiled libCH32H417_TOUCH.a alongside its example, but
 * that library is the filtering and debouncing layer; the measurement itself is
 * the seven lines below, taken from the example's own hardware.c. read() is
 * deliberately the raw number, as it is on ESP32 -- a baseline and a threshold
 * belong to the application, which knows what its pad looks like.
 *
 * Sharing ADC1 with machine.ADC: the TKEY bits are set for the measurement and
 * cleared again afterwards, so the two can be used in any order. The channel
 * table and the converter setup come from machine_adc.c rather than being
 * copied.
 */
#include <stdbool.h>

#include "ch32h417.h"

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/runtime.h"

#include "extmod/modmachine.h"

#include "machine_adc.h"
#include "machine_pin.h"

/* CTLR1 bit 24 enables the touch key function and bit 26 its input buffer.
 * Neither has a name in the SDK header -- the vendor example writes the
 * literals -- and ADC_TKENABLE at bit 24 is the only half of it that does. */
#define TKEY_CTLR1_ENABLE (1u << 24)
#define TKEY_CTLR1_BUFFER (1u << 26)

/* Charge and discharge times, in the units the peripheral counts. These are
 * the vendor example's values. Longer charging gives a bigger reading and more
 * resolution; longer discharging gives more range before the reading
 * saturates. Exposed through the constructor because the right pair depends on
 * the pad: a large plate needs more of both than a wire does. */
#define TKEY_CHARGE_DEFAULT (0x9F)
#define TKEY_DISCHARGE_DEFAULT (0xFF)

typedef struct _machine_touchpad_obj_t {
    mp_obj_base_t base;
    uint8_t pin_id;
    uint8_t channel;
    uint16_t charge;
    uint16_t discharge;
} machine_touchpad_obj_t;

const mp_obj_type_t machine_touchpad_type;

static mp_obj_t machine_touchpad_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_pin, ARG_charge, ARG_discharge };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_pin,       MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_charge,    MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = TKEY_CHARGE_DEFAULT} },
        { MP_QSTR_discharge, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = TKEY_DISCHARGE_DEFAULT} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    const machine_pin_obj_t *pin = machine_pin_get(args[ARG_pin].u_obj);
    uint8_t channel = machine_adc_channel_for_pin(pin->id);
    if (channel == MACHINE_ADC_CHANNEL_NONE) {
        mp_raise_ValueError(MP_ERROR_TEXT("pin has no ADC channel, so no touch input"));
    }
    if (args[ARG_charge].u_int < 1 || args[ARG_charge].u_int > 0xFFFF
        || args[ARG_discharge].u_int < 1 || args[ARG_discharge].u_int > 0xFFFF) {
        mp_raise_ValueError(MP_ERROR_TEXT("charge and discharge must be 1 to 65535"));
    }

    machine_adc_init_hardware();

    /* Analog mode, which disconnects the digital input buffer: it would load
     * the pad and, at the mid-rail voltages a charging pad passes through,
     * burn current oscillating. */
    machine_pin_clock_enable(pin->id);
    GPIO_InitTypeDef gpio = { 0 };
    gpio.GPIO_Pin = MACHINE_PIN_MASK(pin->id);
    gpio.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(machine_pin_gpio(pin->id), &gpio);

    machine_touchpad_obj_t *self = mp_obj_malloc(machine_touchpad_obj_t, &machine_touchpad_type);
    self->pin_id = pin->id;
    self->channel = channel;
    self->charge = (uint16_t)args[ARG_charge].u_int;
    self->discharge = (uint16_t)args[ARG_discharge].u_int;
    return MP_OBJ_FROM_PTR(self);
}

static void machine_touchpad_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    machine_touchpad_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "TouchPad(Pin(P%c%u), channel=%u)",
        'A' + MACHINE_PIN_PORT(self->pin_id), MACHINE_PIN_NUM(self->pin_id), self->channel);
}

/* One charge-discharge measurement.
 *
 * Writing the discharge time is what starts it -- RDATAR is the discharge
 * counter going in and the result coming out, which is why the vendor example
 * writes 0xff to a register it then reads. */
static mp_int_t machine_touchpad_measure(machine_touchpad_obj_t *self) {
    ADC1->CTLR1 |= TKEY_CTLR1_ENABLE | TKEY_CTLR1_BUFFER;
    ADC_LowPowerModeCmd(ADC1, ENABLE);

    ADC_RegularChannelConfig(ADC1, self->channel, 1, ADC_SampleTime_CyclesMode7);
    ADC_ClearFlag(ADC1, ADC_FLAG_EOC);
    TKey1->IDATAR1 = self->charge;
    TKey1->RDATAR = self->discharge;

    mp_uint_t deadline = mp_hal_ticks_ms() + 10;
    while (!ADC_GetFlagStatus(ADC1, ADC_FLAG_EOC)) {
        if (mp_hal_ticks_ms() > deadline) {
            ADC1->CTLR1 &= ~(TKEY_CTLR1_ENABLE | TKEY_CTLR1_BUFFER);
            mp_raise_OSError(MP_ETIMEDOUT);
        }
    }
    mp_int_t value = (mp_int_t)(uint16_t)TKey1->RDATAR;

    /* Leave the converter as an ordinary ADC again, so machine.ADC and
     * machine.TouchPad can be used in either order without one silently
     * reading the other's configuration. */
    ADC1->CTLR1 &= ~(TKEY_CTLR1_ENABLE | TKEY_CTLR1_BUFFER);
    ADC_LowPowerModeCmd(ADC1, DISABLE);
    return value;
}

static mp_obj_t machine_touchpad_read(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(machine_touchpad_measure(MP_OBJ_TO_PTR(self_in)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_touchpad_read_obj, machine_touchpad_read);

static const mp_rom_map_elem_t machine_touchpad_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&machine_touchpad_read_obj) },
};
static MP_DEFINE_CONST_DICT(machine_touchpad_locals_dict, machine_touchpad_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    machine_touchpad_type,
    MP_QSTR_TouchPad,
    MP_TYPE_FLAG_NONE,
    make_new, machine_touchpad_make_new,
    print, machine_touchpad_print,
    locals_dict, &machine_touchpad_locals_dict
    );
