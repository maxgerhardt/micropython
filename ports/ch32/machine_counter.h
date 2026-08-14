/* The parts of machine_counter.c the rest of the port needs. */
#ifndef MICROPY_INCLUDED_CH32_MACHINE_COUNTER_H
#define MICROPY_INCLUDED_CH32_MACHINE_COUNTER_H

#include <stdbool.h>
#include <stdint.h>

/* Count a wrap of the 16-bit hardware counter on timer `id`.
 *
 * Called from the timer interrupt handlers in machine_timer.c, which own the
 * vectors: a timer is either a Timer or a Counter and never both, so one
 * handler per timer serves whichever holds it. Returns true if a Counter did
 * own it, so the handler knows not to look for a Timer as well. */
bool machine_counter_irq(uint8_t id);

/* Stop every counter and release its timer, on soft reset. */
void machine_counter_deinit_all(void);

#endif // MICROPY_INCLUDED_CH32_MACHINE_COUNTER_H
