/* machine.Pin for the CH32H417.
 *
 * Pin ids encode the port and pin number as port_index * 16 + pin_number, so
 * PA9 is 9, PB0 is 16 and PC13 is 45. The encoding is part of the public API:
 * Pin(16) and Pin("PB0") are the same pin.
 */
#ifndef MICROPY_INCLUDED_CH32_MACHINE_PIN_H
#define MICROPY_INCLUDED_CH32_MACHINE_PIN_H

#include "py/obj.h"
#include "py/mphal.h"
#include "ch32h417.h"

#define MACHINE_PIN_PORT(id)      ((id) >> 4)
#define MACHINE_PIN_NUM(id)       ((id) & 0x0f)
#define MACHINE_PIN_ID(port, num) (((port) << 4) | (num))
#define MACHINE_PIN_MASK(id)      ((uint16_t)(1u << MACHINE_PIN_NUM(id)))

#define MACHINE_PIN_PORT_MAX (6)

/* Forward declared as a type in mphalport.h, which py/mphal.h reaches first;
 * this completes it. */
struct _machine_pin_obj_t {
    mp_obj_base_t base;
    uint8_t id;
};

extern const mp_obj_type_t machine_pin_type;

/* The die has 95 I/O ports: PA/PB/PC/PD/PE are complete 16-pin banks and PF
 * stops at PF14. The CH32H417QEU6 is the QFN128 package -- the largest of the
 * family -- and bonds out every one of them, which is why the vendor SDK's
 * GPIO_IPD_Unused() has no case for this part: there are no unused pins to tie
 * down. Smaller packages (MEU/QFN88, WEU/QFN68) do have gaps, so this list must
 * become per-board if such a board is ever added. */
#define MACHINE_PIN_PORT_PINS_0_14(X, p) \
    X(p, 0)  X(p, 1)  X(p, 2)  X(p, 3)   \
    X(p, 4)  X(p, 5)  X(p, 6)  X(p, 7)   \
    X(p, 8)  X(p, 9)  X(p, 10) X(p, 11)  \
    X(p, 12) X(p, 13) X(p, 14)

#define MACHINE_PIN_PORT_PINS_0_15(X, p) \
    MACHINE_PIN_PORT_PINS_0_14(X, p) X(p, 15)

#define MACHINE_PIN_LIST(X)          \
    MACHINE_PIN_PORT_PINS_0_15(X, A) \
    MACHINE_PIN_PORT_PINS_0_15(X, B) \
    MACHINE_PIN_PORT_PINS_0_15(X, C) \
    MACHINE_PIN_PORT_PINS_0_15(X, D) \
    MACHINE_PIN_PORT_PINS_0_15(X, E) \
    MACHINE_PIN_PORT_PINS_0_14(X, F)

/* Declared so other port code (and any future SPI/I2C bit-bang) can name pins
 * directly, in the same way the stm32 port exposes pin_A0 etc. */
#define MACHINE_PIN_DECLARE_OBJ(p, n) extern const machine_pin_obj_t pin_##p##n##_obj;
MACHINE_PIN_LIST(MACHINE_PIN_DECLARE_OBJ)
#undef MACHINE_PIN_DECLARE_OBJ

GPIO_TypeDef * machine_pin_gpio(uint8_t id);
void machine_pin_clock_enable(uint8_t id);

/* Tears down pin interrupts; call before the heap is discarded on soft reset. */
void machine_pin_deinit(void);

/* mp_hal_pin_* is the interface py/ and extmod/ use to touch a pin without
 * knowing the port's object layout. */
#define mp_hal_pin_obj_t const machine_pin_obj_t *
#define mp_hal_get_pin_obj(o) machine_pin_get(o)
#define mp_hal_pin_name(p) ((p)->id)
#define mp_hal_pin_read(p) machine_pin_read(p)
#define mp_hal_pin_write(p, v) machine_pin_write((p), (v))
#define mp_hal_pin_low(p) machine_pin_write((p), 0)
#define mp_hal_pin_high(p) machine_pin_write((p), 1)
#define mp_hal_pin_od_low(p) machine_pin_write((p), 0)
#define mp_hal_pin_od_high(p) machine_pin_write((p), 1)

const machine_pin_obj_t *machine_pin_get(mp_obj_t obj);
int machine_pin_read(const machine_pin_obj_t *self);
void machine_pin_write(const machine_pin_obj_t *self, int value);
void mp_hal_pin_input(const machine_pin_obj_t *self);
void mp_hal_pin_output(const machine_pin_obj_t *self);
void mp_hal_pin_open_drain(const machine_pin_obj_t *self);

#endif // MICROPY_INCLUDED_CH32_MACHINE_PIN_H
