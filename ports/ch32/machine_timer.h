/* The parts of machine_timer.c the rest of the port needs. */
#ifndef MICROPY_INCLUDED_CH32_MACHINE_TIMER_H
#define MICROPY_INCLUDED_CH32_MACHINE_TIMER_H

#include <stdbool.h>
#include <stdint.h>

/* Who owns each of the twelve timers.
 *
 * Four things want them and only one can have each: machine.PWM drives the
 * compare channels, machine.Timer drives the update event, machine.Counter
 * clocks the counter from a pin, and machine.Encoder decodes quadrature into
 * it. All of them set the period, so none can share with another.
 *
 * The table lives in machine_timer.c because that is an ordinary translation
 * unit; machine_pwm.c and machine_counter.c are pasted into their extmod hosts
 * and are static throughout, so neither can hold shared state. PWM claims when
 * its first channel goes up and releases when its last comes down.
 */
enum {
    CH32_TIMER_FREE = 0,
    CH32_TIMER_PWM,
    CH32_TIMER_TIMER,
    CH32_TIMER_COUNTER,
    CH32_TIMER_ENCODER,
    CH32_TIMER_AUDIO,
    CH32_TIMER_ADC,
};

/* Take timer `id` (1-12) for `owner`. False if someone else already has it;
 * re-claiming by the same owner succeeds. */
bool ch32_timer_claim(uint8_t id, uint8_t owner);

/* Give it back. Does nothing if `owner` is not the current holder, so a
 * deinit() that runs twice is harmless. */
void ch32_timer_release(uint8_t id, uint8_t owner);

/* CH32_TIMER_FREE, or one of the owners above. */
uint8_t ch32_timer_owner(uint8_t id);

/* The name of an owner, for error messages: "PWM", "Timer", "Counter",
 * "Encoder", "AudioOut". */
const char *ch32_timer_owner_name(uint8_t owner);

/* Stop every running timer and forget its callback.
 *
 * Called on soft reset, for the same reason machine_pin_deinit() is: the
 * callback is a heap object and the counter keeps running, so one left armed
 * would dispatch into reclaimed memory. */
void machine_timer_deinit_all(void);

#endif // MICROPY_INCLUDED_CH32_MACHINE_TIMER_H
