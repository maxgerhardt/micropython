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

/* Look a pin up in the PWM alternate-function table.
 *
 * machine_encoder.c uses these: quadrature decoding takes TI1 and TI2, the
 * same pads and alternate functions a PWM output would use, so there is no
 * reason for a second table. Negated pins are refused -- TIMx_CHyN is an
 * output of the complementary generator with no input path.
 *
 * machine_pwm_channel_af() answers "can this pin be that timer's channel, and
 * with which AF"; machine_pwm_channel_pin() gives the first pin listed for a
 * channel, for a default. */
bool machine_pwm_channel_af(uint8_t pin, uint8_t timer, uint8_t channel, uint8_t *af);
bool machine_pwm_channel_pin(uint8_t timer, uint8_t channel, uint8_t *pin);

#endif // MICROPY_INCLUDED_CH32_MACHINE_PWM_H
