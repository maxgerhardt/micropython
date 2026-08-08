/* machine.Pin for the CH32H417.
 *
 * The GPIO block is the STM32F1-style one: a 4-bit nibble per pin split across
 * CFGLR (pins 0-7) and CFGHR (pins 8-15), holding a 2-bit MODE (0 = input,
 * non-zero = output) and a 2-bit CNF whose meaning depends on the direction.
 * There is no separate pull-up/pull-down register -- for an input configured
 * with CNF = 10 the pull direction is taken from the output latch, so a pull
 * and an output level are physically the same bit and cannot both be set.
 *
 * The H417 adds a SPEED register (2 bits per pin) on top of that, which is what
 * Pin.drive() controls, and moves alternate-function selection into per-port
 * AFIO registers rather than the F1's fixed remap groups.
 */
#include "ch32h417.h"

#include "py/runtime.h"
#include "py/mphal.h"
#include "py/mperrno.h"
#include "py/gc.h"
#include "extmod/modmachine.h"
/* py/mphal.h only pulls this in when a port does *not* define its own pin API,
 * and this port now does, so the pin protocol types have to come in directly. */
#include "extmod/virtpin.h"

#include "machine_pin.h"

#define PIN_MODE_IN              (0)
#define PIN_MODE_OUT             (1)
#define PIN_MODE_OPEN_DRAIN      (2)
#define PIN_MODE_ALT             (3)
#define PIN_MODE_ALT_OPEN_DRAIN  (4)
#define PIN_MODE_ANALOG          (5)

#define PIN_PULL_NONE (0)
#define PIN_PULL_UP   (1)
#define PIN_PULL_DOWN (2)

#define PIN_IRQ_RISING  (1)
#define PIN_IRQ_FALLING (2)

/* Port index for each letter, used to build the pin objects below. */
#define PIN_PORT_INDEX_A (0)
#define PIN_PORT_INDEX_B (1)
#define PIN_PORT_INDEX_C (2)
#define PIN_PORT_INDEX_D (3)
#define PIN_PORT_INDEX_E (4)
#define PIN_PORT_INDEX_F (5)

static GPIO_TypeDef *const pin_ports[MACHINE_PIN_PORT_MAX] = {
    GPIOA, GPIOB, GPIOC, GPIOD, GPIOE, GPIOF,
};

static const uint32_t pin_port_clk[MACHINE_PIN_PORT_MAX] = {
    RCC_HB2Periph_GPIOA, RCC_HB2Periph_GPIOB, RCC_HB2Periph_GPIOC,
    RCC_HB2Periph_GPIOD, RCC_HB2Periph_GPIOE, RCC_HB2Periph_GPIOF,
};

/* One static object per pin, in flash, so Pin.cpu.PB0 is always the same
 * object and constructing a pin allocates nothing. */
#define MACHINE_PIN_DEFINE_OBJ(p, n)                    \
    const machine_pin_obj_t pin_##p##n##_obj = {  \
        { &machine_pin_type },                          \
        MACHINE_PIN_ID(PIN_PORT_INDEX_##p, n)         \
    };
MACHINE_PIN_LIST(MACHINE_PIN_DEFINE_OBJ)
#undef MACHINE_PIN_DEFINE_OBJ

#define MACHINE_PIN_DICT_ENTRY(p, n) \
    { MP_ROM_QSTR(MP_QSTR_P##p##n), MP_ROM_PTR(&pin_##p##n##_obj) },
static const mp_rom_map_elem_t machine_pin_cpu_locals_dict_table[] = {
    MACHINE_PIN_LIST(MACHINE_PIN_DICT_ENTRY)
};
#undef MACHINE_PIN_DICT_ENTRY

/* The same objects indexed by pin id. The list is emitted in id order and the
 * only gap, PF15, is past the end, so the id indexes this directly. */
#define MACHINE_PIN_PTR_ENTRY(p, n) & pin_##p##n##_obj,
static const machine_pin_obj_t *const machine_pin_by_id[] = {
    MACHINE_PIN_LIST(MACHINE_PIN_PTR_ENTRY)
};
#undef MACHINE_PIN_PTR_ENTRY
static MP_DEFINE_CONST_DICT(machine_pin_cpu_locals_dict, machine_pin_cpu_locals_dict_table);

/* Pin.cpu is a type object whose locals dict holds every pin, so attribute
 * lookup on it resolves names without any runtime table. */
static MP_DEFINE_CONST_OBJ_TYPE(
    machine_pin_cpu_type,
    MP_QSTR_cpu,
    MP_TYPE_FLAG_NONE,
    locals_dict, &machine_pin_cpu_locals_dict
    );

/* EXTI has 16 lines, one per pin number, and each line can be driven by only
 * one port at a time. exti_owner records which pin id currently owns a line so
 * a second pin with the same number cannot silently steal it. */
static uint8_t exti_owner[16];
static bool exti_owned[16];
static bool exti_hard[16];

MP_REGISTER_ROOT_POINTER(mp_obj_t machine_pin_irq_handler[16]);

GPIO_TypeDef *machine_pin_gpio(uint8_t id) {
    return pin_ports[MACHINE_PIN_PORT(id)];
}

void machine_pin_clock_enable(uint8_t id) {
    RCC_HB2PeriphClockCmd(pin_port_clk[MACHINE_PIN_PORT(id)], ENABLE);
}

/* Return the raw 4-bit CFG nibble for a pin. */
static uint32_t pin_cfg_get(const machine_pin_obj_t *self) {
    GPIO_TypeDef *gpio = machine_pin_gpio(self->id);
    uint32_t num = MACHINE_PIN_NUM(self->id);
    uint32_t reg = (num < 8) ? gpio->CFGLR : gpio->CFGHR;
    return (reg >> ((num & 7) * 4)) & 0xf;
}

static bool pin_is_output(const machine_pin_obj_t *self) {
    /* The low two bits of the nibble are MODE; zero means input. */
    return (pin_cfg_get(self) & 3) != 0;
}

int machine_pin_read(const machine_pin_obj_t *self) {
    GPIO_TypeDef *gpio = machine_pin_gpio(self->id);
    uint16_t mask = MACHINE_PIN_MASK(self->id);
    /* Read the output latch for a driven pin so on()/off() read back, and the
     * input register otherwise. Testing OUTDR first would be wrong: for an
     * input with a pull-up the latch is what selects the pull direction, so it
     * reads 1 no matter what the pin is actually sitting at. */
    if (pin_is_output(self)) {
        return (gpio->OUTDR & mask) ? 1 : 0;
    }
    return (gpio->INDR & mask) ? 1 : 0;
}

void machine_pin_write(const machine_pin_obj_t *self, int value) {
    GPIO_TypeDef *gpio = machine_pin_gpio(self->id);
    uint16_t mask = MACHINE_PIN_MASK(self->id);
    if (value) {
        gpio->BSHR = mask;
    } else {
        gpio->BCR = mask;
    }
}

static void pin_apply(const machine_pin_obj_t *self, int mode, int pull, int drive) {
    GPIO_InitTypeDef init = {0};
    init.GPIO_Pin = MACHINE_PIN_MASK(self->id);
    init.GPIO_Speed = (drive < 0) ? GPIO_Speed_High : (GPIOSpeed_TypeDef)drive;

    switch (mode) {
        case PIN_MODE_IN:
            /* Pull and output level share the latch, so the pull is only
             * meaningful here, in an input mode. */
            if (pull == PIN_PULL_UP) {
                init.GPIO_Mode = GPIO_Mode_IPU;
            } else if (pull == PIN_PULL_DOWN) {
                init.GPIO_Mode = GPIO_Mode_IPD;
            } else {
                init.GPIO_Mode = GPIO_Mode_IN_FLOATING;
            }
            break;
        case PIN_MODE_OUT:
            init.GPIO_Mode = GPIO_Mode_Out_PP;
            break;
        case PIN_MODE_OPEN_DRAIN:
            init.GPIO_Mode = GPIO_Mode_Out_OD;
            break;
        case PIN_MODE_ALT:
            init.GPIO_Mode = GPIO_Mode_AF_PP;
            break;
        case PIN_MODE_ALT_OPEN_DRAIN:
            init.GPIO_Mode = GPIO_Mode_AF_OD;
            break;
        case PIN_MODE_ANALOG:
            init.GPIO_Mode = GPIO_Mode_AIN;
            break;
        default:
            mp_raise_ValueError(MP_ERROR_TEXT("invalid pin mode"));
    }

    machine_pin_clock_enable(self->id);
    GPIO_Init(machine_pin_gpio(self->id), &init);
}

const machine_pin_obj_t *machine_pin_get(mp_obj_t obj) {
    if (mp_obj_is_type(obj, &machine_pin_type)) {
        return MP_OBJ_TO_PTR(obj);
    }
    if (mp_obj_is_str(obj)) {
        /* Accept the datasheet spelling, e.g. Pin("PB0"). */
        mp_map_elem_t *elem = mp_map_lookup(
            (mp_map_t *)&machine_pin_cpu_locals_dict.map,
            MP_OBJ_NEW_QSTR(mp_obj_str_get_qstr(obj)), MP_MAP_LOOKUP);
        if (elem != NULL) {
            return MP_OBJ_TO_PTR(elem->value);
        }
        mp_raise_ValueError(MP_ERROR_TEXT("invalid pin"));
    }
    mp_int_t id = mp_obj_get_int(obj);
    if (id < 0 || MACHINE_PIN_PORT(id) >= MACHINE_PIN_PORT_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid pin"));
    }
    /* PF15 is the one hole in the id space: the die stops at PF14. */
    if ((size_t)id >= MP_ARRAY_SIZE(machine_pin_by_id)) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid pin"));
    }
    return machine_pin_by_id[id];
}

void mp_hal_pin_input(const machine_pin_obj_t *self) {
    pin_apply(self, PIN_MODE_IN, PIN_PULL_NONE, -1);
}

void mp_hal_pin_output(const machine_pin_obj_t *self) {
    pin_apply(self, PIN_MODE_OUT, PIN_PULL_NONE, -1);
}

void mp_hal_pin_open_drain(const machine_pin_obj_t *self) {
    pin_apply(self, PIN_MODE_OPEN_DRAIN, PIN_PULL_NONE, -1);
}

static void machine_pin_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    const machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "Pin(P%c%u)", 'A' + MACHINE_PIN_PORT(self->id), MACHINE_PIN_NUM(self->id));
}

enum { ARG_mode, ARG_pull, ARG_value, ARG_drive };
static const mp_arg_t machine_pin_init_args[] = {
    { MP_QSTR_mode,  MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    { MP_QSTR_pull,  MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    { MP_QSTR_value, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    { MP_QSTR_drive, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1} },
};

static mp_obj_t machine_pin_obj_init_helper(const machine_pin_obj_t *self,
    size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    mp_arg_val_t args[MP_ARRAY_SIZE(machine_pin_init_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args,
        MP_ARRAY_SIZE(machine_pin_init_args), machine_pin_init_args, args);

    if (args[ARG_mode].u_obj == mp_const_none) {
        /* Nothing to reconfigure; a bare Pin(id) just names the pin. */
        if (args[ARG_value].u_obj != mp_const_none) {
            machine_pin_write(self, mp_obj_is_true(args[ARG_value].u_obj));
        }
        return mp_const_none;
    }

    int mode = mp_obj_get_int(args[ARG_mode].u_obj);
    int pull = PIN_PULL_NONE;
    if (args[ARG_pull].u_obj != mp_const_none) {
        pull = mp_obj_get_int(args[ARG_pull].u_obj);
    }

    /* Set the latch before switching the pin to an output, so a pin that is
     * about to drive does not first emit whatever the latch happened to hold. */
    if (args[ARG_value].u_obj != mp_const_none) {
        machine_pin_write(self, mp_obj_is_true(args[ARG_value].u_obj));
    }
    pin_apply(self, mode, pull, args[ARG_drive].u_int);
    return mp_const_none;
}

static mp_obj_t machine_pin_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, MP_OBJ_FUN_ARGS_MAX, true);
    const machine_pin_obj_t *self = machine_pin_get(args[0]);

    if (n_args > 1 || n_kw > 0) {
        mp_map_t kw_args;
        mp_map_init_fixed_table(&kw_args, n_kw, args + n_args);
        machine_pin_obj_init_helper(self, n_args - 1, args + 1, &kw_args);
    }
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t machine_pin_obj_init(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    return machine_pin_obj_init_helper(MP_OBJ_TO_PTR(args[0]), n_args - 1, args + 1, kw_args);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(machine_pin_init_obj, 1, machine_pin_obj_init);

static mp_obj_t machine_pin_call(mp_obj_t self_in, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 1, false);
    const machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (n_args == 0) {
        return MP_OBJ_NEW_SMALL_INT(machine_pin_read(self));
    }
    machine_pin_write(self, mp_obj_is_true(args[0]));
    return mp_const_none;
}

static mp_obj_t machine_pin_value(size_t n_args, const mp_obj_t *args) {
    return machine_pin_call(args[0], n_args - 1, 0, args + 1);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_pin_value_obj, 1, 2, machine_pin_value);

static mp_obj_t machine_pin_on(mp_obj_t self_in) {
    machine_pin_write(MP_OBJ_TO_PTR(self_in), 1);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_pin_on_obj, machine_pin_on);

static mp_obj_t machine_pin_off(mp_obj_t self_in) {
    machine_pin_write(MP_OBJ_TO_PTR(self_in), 0);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_pin_off_obj, machine_pin_off);

static mp_obj_t machine_pin_toggle(mp_obj_t self_in) {
    const machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    /* Toggle what the pin drives, which is the latch, not the input level. */
    GPIO_TypeDef *gpio = machine_pin_gpio(self->id);
    uint16_t mask = MACHINE_PIN_MASK(self->id);
    machine_pin_write(self, (gpio->OUTDR & mask) ? 0 : 1);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_pin_toggle_obj, machine_pin_toggle);

static mp_obj_t machine_pin_mode(size_t n_args, const mp_obj_t *args) {
    const machine_pin_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    if (n_args == 1) {
        uint32_t cfg = pin_cfg_get(self);
        uint32_t cnf = cfg >> 2;
        if ((cfg & 3) == 0) {
            static const uint8_t in_modes[4] = {
                PIN_MODE_ANALOG, PIN_MODE_IN, PIN_MODE_IN, PIN_MODE_IN
            };
            return MP_OBJ_NEW_SMALL_INT(in_modes[cnf]);
        }
        static const uint8_t out_modes[4] = {
            PIN_MODE_OUT, PIN_MODE_OPEN_DRAIN, PIN_MODE_ALT, PIN_MODE_ALT_OPEN_DRAIN
        };
        return MP_OBJ_NEW_SMALL_INT(out_modes[cnf]);
    }
    pin_apply(self, mp_obj_get_int(args[1]), PIN_PULL_NONE, -1);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_pin_mode_obj, 1, 2, machine_pin_mode);

static mp_obj_t machine_pin_pull(size_t n_args, const mp_obj_t *args) {
    const machine_pin_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    if (n_args == 1) {
        uint32_t cfg = pin_cfg_get(self);
        /* Only an input with CNF = 10 has a pull, and its direction is the
         * output latch. */
        if ((cfg & 3) != 0 || (cfg >> 2) != 2) {
            return mp_const_none;
        }
        GPIO_TypeDef *gpio = machine_pin_gpio(self->id);
        return MP_OBJ_NEW_SMALL_INT((gpio->OUTDR & MACHINE_PIN_MASK(self->id))
            ? PIN_PULL_UP : PIN_PULL_DOWN);
    }
    int pull = (args[1] == mp_const_none) ? PIN_PULL_NONE : mp_obj_get_int(args[1]);
    pin_apply(self, PIN_MODE_IN, pull, -1);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_pin_pull_obj, 1, 2, machine_pin_pull);

static mp_obj_t machine_pin_drive(size_t n_args, const mp_obj_t *args) {
    const machine_pin_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    GPIO_TypeDef *gpio = machine_pin_gpio(self->id);
    uint32_t shift = MACHINE_PIN_NUM(self->id) * 2;
    if (n_args == 1) {
        return MP_OBJ_NEW_SMALL_INT((gpio->SPEED >> shift) & 3);
    }
    uint32_t drive = mp_obj_get_int(args[1]) & 3;
    gpio->SPEED = (gpio->SPEED & ~(3u << shift)) | (drive << shift);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_pin_drive_obj, 1, 2, machine_pin_drive);

/* --- interrupts --- */

static void pin_exti_disable(uint32_t line) {
    EXTI->INTENR &= ~(1u << line);
    EXTI->RTENR &= ~(1u << line);
    EXTI->FTENR &= ~(1u << line);
    EXTI->INTFR = 1u << line;
}

/* Called on soft reset, before the heap holding the handlers goes away.
 *
 * Without this a Ctrl-D leaves every EXTI line still enabled and pointing at a
 * handler the GC is about to reclaim, and leaves exti_owner[] claiming lines
 * nothing owns any more -- so the first irq() after a soft reset would fail
 * with "EXTI line in use" for no visible reason. */
void machine_pin_deinit(void) {
    for (uint32_t line = 0; line < 16; line++) {
        if (exti_owned[line]) {
            pin_exti_disable(line);
            exti_owned[line] = false;
        }
        MP_STATE_PORT(machine_pin_irq_handler)[line] = MP_OBJ_NULL;
    }
}

static mp_obj_t machine_pin_irq(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_handler, ARG_trigger, ARG_hard };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_handler, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_trigger, MP_ARG_INT, {.u_int = PIN_IRQ_RISING | PIN_IRQ_FALLING} },
        { MP_QSTR_hard,    MP_ARG_BOOL, {.u_bool = false} },
    };
    const machine_pin_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    uint32_t line = MACHINE_PIN_NUM(self->id);

    if (args[ARG_handler].u_obj == mp_const_none) {
        if (exti_owned[line] && exti_owner[line] == self->id) {
            pin_exti_disable(line);
            exti_owned[line] = false;
            MP_STATE_PORT(machine_pin_irq_handler)[line] = MP_OBJ_NULL;
        }
        return mp_const_none;
    }

    /* Each EXTI line is shared by the same pin number across all six ports, so
     * only one of PA0/PB0/../PF0 can have an interrupt at a time. Refuse rather
     * than silently retargeting a line another pin is already using. */
    if (exti_owned[line] && exti_owner[line] != self->id) {
        mp_raise_ValueError(MP_ERROR_TEXT("EXTI line in use by another port"));
    }

    RCC_HB2PeriphClockCmd(RCC_HB2Periph_AFIO, ENABLE);

    /* Route the line to this pin's port: 4 bits per line, 8 lines per register. */
    __IO uint32_t *exticr = (line < 8) ? &AFIO->EXTICR1 : &AFIO->EXTICR2;
    uint32_t shift = (line & 7) * 4;
    *exticr = (*exticr & ~(0xfu << shift)) | ((uint32_t)MACHINE_PIN_PORT(self->id) << shift);

    MP_STATE_PORT(machine_pin_irq_handler)[line] = args[ARG_handler].u_obj;
    exti_owner[line] = self->id;
    exti_owned[line] = true;
    exti_hard[line] = args[ARG_hard].u_bool;

    uint32_t bit = 1u << line;
    int trigger = args[ARG_trigger].u_int;
    if (trigger & PIN_IRQ_RISING) {
        EXTI->RTENR |= bit;
    } else {
        EXTI->RTENR &= ~bit;
    }
    if (trigger & PIN_IRQ_FALLING) {
        EXTI->FTENR |= bit;
    } else {
        EXTI->FTENR &= ~bit;
    }
    EXTI->INTFR = bit;
    EXTI->INTENR |= bit;

    NVIC_EnableIRQ((line < 8) ? EXTI7_0_IRQn : EXTI15_8_IRQn);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(machine_pin_irq_obj, 1, machine_pin_irq);

static void pin_exti_service(uint32_t first, uint32_t last) {
    for (uint32_t line = first; line <= last; line++) {
        uint32_t bit = 1u << line;
        if (!(EXTI->INTFR & bit)) {
            continue;
        }
        EXTI->INTFR = bit;
        mp_obj_t handler = MP_STATE_PORT(machine_pin_irq_handler)[line];
        if (handler == MP_OBJ_NULL) {
            continue;
        }
        const machine_pin_obj_t *pin = machine_pin_by_id[exti_owner[line]];
        if (exti_hard[line]) {
            mp_sched_lock();
            gc_lock();
            nlr_buf_t nlr;
            if (nlr_push(&nlr) == 0) {
                mp_call_function_1(handler, MP_OBJ_FROM_PTR(pin));
                nlr_pop();
            } else {
                /* A hard handler cannot propagate an exception anywhere useful,
                 * so drop the interrupt rather than unwinding through the ISR. */
                MP_STATE_PORT(machine_pin_irq_handler)[line] = MP_OBJ_NULL;
                pin_exti_disable(line);
                mp_printf(MICROPY_ERROR_PRINTER, "uncaught exception in Pin irq\n");
                mp_obj_print_exception(MICROPY_ERROR_PRINTER, MP_OBJ_FROM_PTR(nlr.ret_val));
            }
            gc_unlock();
            mp_sched_unlock();
        } else {
            mp_sched_schedule(handler, MP_OBJ_FROM_PTR(pin));
        }
    }
}

void EXTI7_0_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void EXTI7_0_IRQHandler(void) {
    pin_exti_service(0, 7);
}

void EXTI15_8_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void EXTI15_8_IRQHandler(void) {
    pin_exti_service(8, 15);
}

/* --- type --- */

static mp_uint_t machine_pin_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode) {
    (void)errcode;
    const machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    switch (request) {
        case MP_PIN_READ:
            return machine_pin_read(self);
        case MP_PIN_WRITE:
            machine_pin_write(self, arg);
            return 0;
    }
    return -1;
}

static const mp_pin_p_t machine_pin_pin_p = {
    .ioctl = machine_pin_ioctl,
};

static const mp_rom_map_elem_t machine_pin_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_init),   MP_ROM_PTR(&machine_pin_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_value),  MP_ROM_PTR(&machine_pin_value_obj) },
    { MP_ROM_QSTR(MP_QSTR_on),     MP_ROM_PTR(&machine_pin_on_obj) },
    { MP_ROM_QSTR(MP_QSTR_off),    MP_ROM_PTR(&machine_pin_off_obj) },
    { MP_ROM_QSTR(MP_QSTR_toggle), MP_ROM_PTR(&machine_pin_toggle_obj) },
    { MP_ROM_QSTR(MP_QSTR_mode),   MP_ROM_PTR(&machine_pin_mode_obj) },
    { MP_ROM_QSTR(MP_QSTR_pull),   MP_ROM_PTR(&machine_pin_pull_obj) },
    { MP_ROM_QSTR(MP_QSTR_drive),  MP_ROM_PTR(&machine_pin_drive_obj) },
    { MP_ROM_QSTR(MP_QSTR_irq),    MP_ROM_PTR(&machine_pin_irq_obj) },

    { MP_ROM_QSTR(MP_QSTR_cpu),    MP_ROM_PTR(&machine_pin_cpu_type) },

    { MP_ROM_QSTR(MP_QSTR_IN),              MP_ROM_INT(PIN_MODE_IN) },
    { MP_ROM_QSTR(MP_QSTR_OUT),             MP_ROM_INT(PIN_MODE_OUT) },
    { MP_ROM_QSTR(MP_QSTR_OPEN_DRAIN),      MP_ROM_INT(PIN_MODE_OPEN_DRAIN) },
    { MP_ROM_QSTR(MP_QSTR_ALT),             MP_ROM_INT(PIN_MODE_ALT) },
    { MP_ROM_QSTR(MP_QSTR_ALT_OPEN_DRAIN),  MP_ROM_INT(PIN_MODE_ALT_OPEN_DRAIN) },
    { MP_ROM_QSTR(MP_QSTR_ANALOG),          MP_ROM_INT(PIN_MODE_ANALOG) },

    { MP_ROM_QSTR(MP_QSTR_PULL_UP),   MP_ROM_INT(PIN_PULL_UP) },
    { MP_ROM_QSTR(MP_QSTR_PULL_DOWN), MP_ROM_INT(PIN_PULL_DOWN) },

    { MP_ROM_QSTR(MP_QSTR_IRQ_RISING),  MP_ROM_INT(PIN_IRQ_RISING) },
    { MP_ROM_QSTR(MP_QSTR_IRQ_FALLING), MP_ROM_INT(PIN_IRQ_FALLING) },

    { MP_ROM_QSTR(MP_QSTR_DRIVE_0), MP_ROM_INT(GPIO_Speed_Low) },
    { MP_ROM_QSTR(MP_QSTR_DRIVE_1), MP_ROM_INT(GPIO_Speed_Medium) },
    { MP_ROM_QSTR(MP_QSTR_DRIVE_2), MP_ROM_INT(GPIO_Speed_High) },
    { MP_ROM_QSTR(MP_QSTR_DRIVE_3), MP_ROM_INT(GPIO_Speed_Very_High) },
};
static MP_DEFINE_CONST_DICT(machine_pin_locals_dict, machine_pin_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    machine_pin_type,
    MP_QSTR_Pin,
    MP_TYPE_FLAG_NONE,
    make_new, machine_pin_make_new,
    print, machine_pin_print,
    call, machine_pin_call,
    protocol, &machine_pin_pin_p,
    locals_dict, &machine_pin_locals_dict
    );
