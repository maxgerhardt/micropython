/* The parts of machine_encoder.c the rest of the port needs. */
#ifndef MICROPY_INCLUDED_CH32_MACHINE_ENCODER_H
#define MICROPY_INCLUDED_CH32_MACHINE_ENCODER_H

#include <stdbool.h>
#include <stdint.h>

/* Count a wrap of the 16-bit counter on timer `id`, in whichever direction the
 * encoder is currently turning. Called from the timer interrupt handlers in
 * machine_timer.c, which own the vectors; returns true if an Encoder held the
 * timer, so the handler stops looking. */
bool machine_encoder_irq(uint8_t id);

/* Stop every encoder and release its timer, on soft reset. */
void machine_encoder_deinit_all(void);

#endif // MICROPY_INCLUDED_CH32_MACHINE_ENCODER_H
