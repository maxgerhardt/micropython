#ifndef MICROPY_INCLUDED_CH32_MPHALPORT_H
#define MICROPY_INCLUDED_CH32_MPHALPORT_H

#include <stdint.h>
#include <stddef.h>
#include "py/mpconfig.h"
/* For mp_obj_t, needed by the pin API below. py/mphal.h includes this header
 * before anything else has defined it, and py/obj.h does not include mphal.h,
 * so there is no cycle. */
#include "py/obj.h"
#include "py/ringbuf.h"
#include "shared/runtime/interrupt_char.h"

/* Every console feeds one buffer: the USART1 ISR and, when enumerated, the USB
 * CDC layer in shared/tinyusb. mp_hal_stdin_rx_chr drains it, so input is
 * accepted from whichever console the user is actually on. */
extern ringbuf_t stdin_ringbuf;

static inline void mp_hal_wake_main_task_from_isr(void) {
    /* Nothing to wake: this port has no scheduler thread and the REPL polls. */
}

// This header is included from py/mphal.h, which already declares the standard
// mp_hal_* API. Only port-specific additions belong here.

/* Pin HAL.
 *
 * These have to be defined before py/mphal.h falls back to the core's generic
 * "virtual pin" API, and py/mphal.h includes this header before that check --
 * so they live here rather than in machine_pin.h, which is included too late
 * and would only redefine them.
 *
 * The object is defined here rather than in machine_pin.h because extmod's
 * SoftI2C and SoftSPI reach it through mp_hal_pin_name() without including any
 * port header, so a forward declaration is not enough -- they need the
 * complete type. */
typedef struct _machine_pin_obj_t {
    mp_obj_base_t base;
    uint8_t id;                       /* port index * 16 + pin number */
} machine_pin_obj_t;

#define mp_hal_pin_obj_t const machine_pin_obj_t *
#define mp_hal_get_pin_obj(o) machine_pin_get(o)
#define mp_hal_pin_name(p) ((p)->id)
#define mp_hal_pin_read(p) machine_pin_read(p)
#define mp_hal_pin_write(p, v) machine_pin_write((p), (v))
#define mp_hal_pin_low(p) machine_pin_write((p), 0)
#define mp_hal_pin_high(p) machine_pin_write((p), 1)
#define mp_hal_pin_od_low(p) machine_pin_write((p), 0)
#define mp_hal_pin_od_high(p) machine_pin_write((p), 1)

/* mp_hal_pin_name() yields the pin id, so extmod prints it as a plain number. */
#define MP_HAL_PIN_FMT "%u"

int machine_pin_read(const machine_pin_obj_t *self);
void machine_pin_write(const machine_pin_obj_t *self, int value);
void mp_hal_pin_input(const machine_pin_obj_t *self);
void mp_hal_pin_output(const machine_pin_obj_t *self);
void mp_hal_pin_open_drain(const machine_pin_obj_t *self);
const machine_pin_obj_t *machine_pin_get(mp_obj_t obj);

/* SoftI2C/SoftSPI bit-bang with this. There is no faster path than the plain
 * microsecond delay here -- it is already a bare SysTick read loop. */
#define mp_hal_delay_us_fast mp_hal_delay_us

// Provided by the SDK's system_ch32h417.c.
extern uint32_t SystemCoreClock;

void mp_hal_init(void);

#endif // MICROPY_INCLUDED_CH32_MPHALPORT_H
