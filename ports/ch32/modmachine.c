/* machine module for the CH32H417.
 * Included by extmod/modmachine.c via MICROPY_PY_MACHINE_INCLUDEFILE, so the
 * mp_machine_* hooks below must keep the static linkage declared there. */
#include "ch32h417.h"
#include "py/runtime.h"
#include "py/mphal.h"

/* Pin ids encode port and pin as port_index * 16 + pin_number, so PA9 is 9,
 * PB0 is 16 and PC13 is 45. */
#define PIN_PORT(id) ((id) / 16)
#define PIN_NUM(id)  ((id) % 16)

#define PIN_MODE_IN  (0)
#define PIN_MODE_OUT (1)

typedef struct _machine_pin_obj_t {
    mp_obj_base_t base;
    uint8_t id;
} machine_pin_obj_t;

static GPIO_TypeDef *const pin_ports[] = {GPIOA, GPIOB, GPIOC, GPIOD, GPIOE, GPIOF};
static const uint32_t pin_port_clk[] = {
    RCC_HB2Periph_GPIOA, RCC_HB2Periph_GPIOB, RCC_HB2Periph_GPIOC,
    RCC_HB2Periph_GPIOD, RCC_HB2Periph_GPIOE, RCC_HB2Periph_GPIOF,
};

static void machine_pin_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "Pin(%c%u)", 'A' + PIN_PORT(self->id), PIN_NUM(self->id));
}

static mp_obj_t machine_pin_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, 3, false);

    mp_int_t id = mp_obj_get_int(args[0]);
    if (id < 0 || (size_t)PIN_PORT(id) >= MP_ARRAY_SIZE(pin_ports)) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid pin"));
    }

    machine_pin_obj_t *self = mp_obj_malloc(machine_pin_obj_t, type);
    self->id = (uint8_t)id;

    RCC_HB2PeriphClockCmd(pin_port_clk[PIN_PORT(id)], ENABLE);

    if (n_args >= 2) {
        GPIO_InitTypeDef init = {0};
        init.GPIO_Pin = (uint16_t)(1u << PIN_NUM(id));
        init.GPIO_Speed = GPIO_Speed_High;
        init.GPIO_Mode = (mp_obj_get_int(args[1]) == PIN_MODE_OUT)
            ? GPIO_Mode_Out_PP : GPIO_Mode_IN_FLOATING;
        GPIO_Init(pin_ports[PIN_PORT(id)], &init);
    }
    if (n_args >= 3) {
        GPIO_WriteBit(pin_ports[PIN_PORT(id)], (uint16_t)(1u << PIN_NUM(id)),
            mp_obj_is_true(args[2]) ? Bit_SET : Bit_RESET);
    }
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t machine_pin_value(size_t n_args, const mp_obj_t *args) {
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    GPIO_TypeDef *port = pin_ports[PIN_PORT(self->id)];
    uint16_t mask = (uint16_t)(1u << PIN_NUM(self->id));
    if (n_args == 1) {
        /* Read the output latch for driven pins so on()/off() read back, and
         * the input register otherwise. */
        uint8_t v = (port->OUTDR & mask) ? 1 : ((port->INDR & mask) ? 1 : 0);
        return MP_OBJ_NEW_SMALL_INT(v);
    }
    GPIO_WriteBit(port, mask, mp_obj_is_true(args[1]) ? Bit_SET : Bit_RESET);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_pin_value_obj, 1, 2, machine_pin_value);

static mp_obj_t machine_pin_on(mp_obj_t self_in) {
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    GPIO_WriteBit(pin_ports[PIN_PORT(self->id)],
        (uint16_t)(1u << PIN_NUM(self->id)), Bit_SET);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_pin_on_obj, machine_pin_on);

static mp_obj_t machine_pin_off(mp_obj_t self_in) {
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    GPIO_WriteBit(pin_ports[PIN_PORT(self->id)],
        (uint16_t)(1u << PIN_NUM(self->id)), Bit_RESET);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_pin_off_obj, machine_pin_off);

static const mp_rom_map_elem_t machine_pin_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_value), MP_ROM_PTR(&machine_pin_value_obj) },
    { MP_ROM_QSTR(MP_QSTR_on),    MP_ROM_PTR(&machine_pin_on_obj) },
    { MP_ROM_QSTR(MP_QSTR_off),   MP_ROM_PTR(&machine_pin_off_obj) },
    { MP_ROM_QSTR(MP_QSTR_IN),    MP_ROM_INT(PIN_MODE_IN) },
    { MP_ROM_QSTR(MP_QSTR_OUT),   MP_ROM_INT(PIN_MODE_OUT) },
};
static MP_DEFINE_CONST_DICT(machine_pin_locals_dict, machine_pin_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    machine_pin_type,
    MP_QSTR_Pin,
    MP_TYPE_FLAG_NONE,
    make_new, machine_pin_make_new,
    print, machine_pin_print,
    locals_dict, &machine_pin_locals_dict
    );

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

static mp_int_t mp_machine_reset_cause(void) {
    return 0;
}

#define MICROPY_PY_MACHINE_EXTRA_GLOBALS \
    { MP_ROM_QSTR(MP_QSTR_Pin), MP_ROM_PTR(&machine_pin_type) },
