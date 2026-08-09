/* The one part of machine_pwm.c that the rest of the port needs.
 *
 * machine_pwm.c itself is pasted into extmod/machine_pwm.c through
 * MICROPY_PY_MACHINE_PWM_INCLUDEFILE, so everything in it is static; this
 * declaration exists so main.c can stop the outputs on a soft reset without
 * reaching into that file.
 */
#ifndef MICROPY_INCLUDED_CH32_MACHINE_PWM_H
#define MICROPY_INCLUDED_CH32_MACHINE_PWM_H

/* Stop every running PWM output and release its pin.
 *
 * Called on soft reset. Without it Ctrl-D would leave a motor driver or a
 * servo running at whatever duty the program that just died had set, with
 * nothing left on the board that remembers the pin is driven.
 */
void machine_pwm_deinit_all(void);

#endif // MICROPY_INCLUDED_CH32_MACHINE_PWM_H
