/* The parts of machine_timer.c the rest of the port needs. */
#ifndef MICROPY_INCLUDED_CH32_MACHINE_TIMER_H
#define MICROPY_INCLUDED_CH32_MACHINE_TIMER_H

#include <stdbool.h>
#include <stdint.h>

/* True if machine.Timer owns this timer (1-12).
 *
 * machine_pwm.c asks before claiming a channel: a Timer drives the update
 * event and therefore owns the period, which is not something a PWM output can
 * share. The reverse question is machine_pwm_timer_in_use(). */
bool machine_timer_owns(uint8_t id);

/* Stop every running timer and forget its callback.
 *
 * Called on soft reset, for the same reason machine_pin_deinit() is: the
 * callback is a heap object and the counter keeps running, so one left armed
 * would dispatch into reclaimed memory. */
void machine_timer_deinit_all(void);

#endif // MICROPY_INCLUDED_CH32_MACHINE_TIMER_H
