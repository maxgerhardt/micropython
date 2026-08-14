/* The one part of machine_pwm.c that the rest of the port needs.
 *
 * machine_pwm.c itself is pasted into extmod/machine_pwm.c through
 * MICROPY_PY_MACHINE_PWM_INCLUDEFILE, so everything in it is static; this
 * declaration exists so main.c can stop the outputs on a soft reset without
 * reaching into that file.
 */
#ifndef MICROPY_INCLUDED_CH32_MACHINE_PWM_H
#define MICROPY_INCLUDED_CH32_MACHINE_PWM_H

#include <stdbool.h>
#include <stdint.h>

/* Stop every running PWM output and release its pin.
 *
 * Called on soft reset. Without it Ctrl-D would leave a motor driver or a
 * servo running at whatever duty the program that just died had set, with
 * nothing left on the board that remembers the pin is driven.
 */
void machine_pwm_deinit_all(void);

/* True if any PWM channel is running on this timer (1-12).
 *
 * machine.Timer asks before claiming one, because a Timer drives the update
 * event and so owns the period, which a PWM output cannot share. The reverse
 * question is machine_timer_owns(). The two arbitrate by asking each other
 * rather than through a common allocator, because machine_pwm.c is pasted into
 * extmod/machine_pwm.c and has no external symbols to share a table through.
 */
bool machine_pwm_timer_in_use(uint8_t timer);

#endif // MICROPY_INCLUDED_CH32_MACHINE_PWM_H
