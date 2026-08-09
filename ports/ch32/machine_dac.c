/* machine.DAC for the CH32H417 -- a real analog output, not a filtered PWM.
 *
 * Two independent 12-bit converters with their own output amplifiers:
 *
 *     DAC1 -> PA4      DAC2 -> PA5
 *
 * Those pins are fixed in silicon. There is no mux to move them, so a board
 * that needs the analog output has to leave PA4 or PA5 alone -- on this one
 * they are otherwise SPI1's NSS and SCK.
 *
 * The output amplifier is enabled. It can drive a 5 kohm load, which is what
 * makes the output usable by anything other than a very high impedance input,
 * and unlike the STM32 part this one is nearly rail to rail with the buffer
 * on: the datasheet gives 0-8 mV at code 0 and 3.29-3.3 V at code 4095 against
 * a 3.3 V VREFP. Buffer off would gain 5 mV at the bottom and cost the ability
 * to drive anything.
 *
 * Full scale follows VREFP, not VDD33A, and the two are tied together on this
 * board.
 */
#include "ch32h417.h"

#include "py/mphal.h"
#include "py/runtime.h"

#include "machine_pin.h"
#include "machine_dac.h"

#define DAC_BITS      (12)
#define DAC_MAX       ((1 << DAC_BITS) - 1)

/* Full scale in microvolts. The reference is the 3.3 V analog rail, the same
 * assumption machine_adc.c's read_uv() makes, so a DAC output looped back into
 * an ADC input compares directly. */
#define DAC_VREF_UV   (3300000)

typedef struct _machine_dac_obj_t {
    mp_obj_base_t base;
    uint8_t id;             /* 1 or 2 */
    uint8_t pin_id;
    bool active;
} machine_dac_obj_t;

/* Which pin each converter comes out on. Fixed function, no alternate. */
static uint8_t machine_dac_pin_for(uint8_t id) {
    /* PA4 is pin 4 of port A, PA5 is pin 5. */
    return MACHINE_PIN_ID(0, id == 1 ? 4 : 5);
}

static uint32_t machine_dac_channel(uint8_t id) {
    return id == 1 ? DAC_Channel_1 : DAC_Channel_2;
}

static void machine_dac_write_raw(machine_dac_obj_t *self, uint16_t value) {
    if (self->id == 1) {
        DAC_SetChannel1Data(DAC_Align_12b_R, value);
    } else {
        DAC_SetChannel2Data(DAC_Align_12b_R, value);
    }
}

static void machine_dac_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    machine_dac_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->active) {
        mp_printf(print, "DAC(%u)", (unsigned)self->id);
        return;
    }
    uint16_t value = DAC_GetDataOutputValue(machine_dac_channel(self->id));
    mp_printf(print, "DAC(%u, pin=PA%u, bits=%u, value=%u)",
        (unsigned)self->id, (unsigned)MACHINE_PIN_NUM(self->pin_id),
        (unsigned)DAC_BITS, (unsigned)value);
}

/* DAC(1) / DAC(2), or DAC(Pin("PA4")) -- machine.DAC is documented as taking
 * either an index or a pin depending on the port, and the mapping here is
 * one to one, so both work. */
static mp_obj_t machine_dac_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, 1, false);

    uint8_t id;
    if (mp_obj_is_int(args[0])) {
        id = (uint8_t)mp_obj_get_int(args[0]);
        if (id != 1 && id != 2) {
            mp_raise_ValueError(MP_ERROR_TEXT("DAC must be 1 or 2"));
        }
    } else {
        const machine_pin_obj_t *pin = machine_pin_get(args[0]);
        if (pin->id == MACHINE_PIN_ID(0, 4)) {
            id = 1;
        } else if (pin->id == MACHINE_PIN_ID(0, 5)) {
            id = 2;
        } else {
            mp_raise_ValueError(MP_ERROR_TEXT("pin has no DAC; use PA4 or PA5"));
        }
    }

    uint8_t pin_id = machine_dac_pin_for(id);

    /* Analog mode: the pad's digital input buffer and both drivers have to be
     * out of the way, or they load the converter's own output. */
    machine_pin_clock_enable(pin_id);
    GPIO_InitTypeDef gpio = {0};
    gpio.GPIO_Pin = MACHINE_PIN_MASK(pin_id);
    gpio.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(machine_pin_gpio(pin_id), &gpio);

    RCC_HB1PeriphClockCmd(RCC_HB1Periph_DAC, ENABLE);

    DAC_InitTypeDef init;
    DAC_StructInit(&init);
    /* No trigger: the conversion starts as soon as the holding register is
     * written, which is what write() should mean. */
    init.DAC_Trigger = DAC_Trigger_None;
    init.DAC_WaveGeneration = DAC_WaveGeneration_None;
    init.DAC_OutputBuffer = DAC_OutputBuffer_Enable;
    DAC_Init(machine_dac_channel(id), &init);

    machine_dac_obj_t *self = mp_obj_malloc(machine_dac_obj_t, type);
    self->id = id;
    self->pin_id = pin_id;
    self->active = true;

    /* Start at zero rather than at whatever the holding register held from a
     * previous run: enabling the channel drives the pin immediately. */
    machine_dac_write_raw(self, 0);
    DAC_Cmd(machine_dac_channel(id), ENABLE);
    return MP_OBJ_FROM_PTR(self);
}

void machine_dac_deinit_all(void) {
    /* Both channels, unconditionally: a DAC object lives on the heap that the
     * soft reset is about to discard, but the converter keeps driving its pin
     * whatever happens to the object. Disabling a channel that was never
     * enabled is harmless, and reaching the register needs the clock, which
     * DAC_Cmd cannot turn on for itself. */
    RCC_HB1PeriphClockCmd(RCC_HB1Periph_DAC, ENABLE);
    DAC_Cmd(DAC_Channel_1, DISABLE);
    DAC_Cmd(DAC_Channel_2, DISABLE);
}

static machine_dac_obj_t *machine_dac_check(mp_obj_t self_in) {
    machine_dac_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->active) {
        mp_raise_ValueError(MP_ERROR_TEXT("DAC is deinitialised"));
    }
    return self;
}

// DAC.write(value) -- 0 to 4095.
static mp_obj_t machine_dac_write(mp_obj_t self_in, mp_obj_t value_in) {
    machine_dac_obj_t *self = machine_dac_check(self_in);
    mp_int_t value = mp_obj_get_int(value_in);
    if (value < 0 || value > DAC_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("value must be 0 to 4095"));
    }
    machine_dac_write_raw(self, (uint16_t)value);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(machine_dac_write_obj, machine_dac_write);

/* DAC.write_uv(microvolts). Not part of the machine.DAC API, but this port's
 * ADC has read_uv(), and a converter whose full scale is only implied by its
 * bit count is awkward to use against one that reports volts. Rounds to the
 * nearest code, so the value read back from write_uv/read_uv round trips as
 * closely as 12 bits allow. */
static mp_obj_t machine_dac_write_uv(mp_obj_t self_in, mp_obj_t uv_in) {
    machine_dac_obj_t *self = machine_dac_check(self_in);
    mp_int_t uv = mp_obj_get_int(uv_in);
    if (uv < 0 || uv > DAC_VREF_UV) {
        mp_raise_ValueError(MP_ERROR_TEXT("microvolts outside 0 to 3300000"));
    }
    uint32_t code = ((uint64_t)uv * DAC_MAX + DAC_VREF_UV / 2) / DAC_VREF_UV;
    machine_dac_write_raw(self, (uint16_t)code);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(machine_dac_write_uv_obj, machine_dac_write_uv);

// DAC.deinit() -- stop driving the pin.
static mp_obj_t machine_dac_deinit(mp_obj_t self_in) {
    machine_dac_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->active) {
        DAC_Cmd(machine_dac_channel(self->id), DISABLE);
        self->active = false;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_dac_deinit_obj, machine_dac_deinit);

static const mp_rom_map_elem_t machine_dac_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&machine_dac_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_write_uv), MP_ROM_PTR(&machine_dac_write_uv_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&machine_dac_deinit_obj) },
};
static MP_DEFINE_CONST_DICT(machine_dac_locals_dict, machine_dac_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    machine_dac_type,
    MP_QSTR_DAC,
    MP_TYPE_FLAG_NONE,
    make_new, machine_dac_make_new,
    print, machine_dac_print,
    locals_dict, &machine_dac_locals_dict
    );
