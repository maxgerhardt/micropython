#ifndef MICROPY_INCLUDED_CH32_MACHINE_SLEEP_H
#define MICROPY_INCLUDED_CH32_MACHINE_SLEEP_H

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"

/* True if this boot is the far side of machine.deepsleep(). Reads and clears
 * the flag, so it must be called exactly once, early, before anything can
 * observe it. */
bool machine_sleep_deepsleep_flag_take(void);

/* Sleep mode: core clock off, every peripheral clock still running, RAM and
 * registers untouched, execution resumes where it left off. ms < 0 sleeps
 * until any interrupt. */
void machine_sleep_light(mp_int_t ms);

/* Stop mode: every clock in the chip stops. Never returns -- the chip is reset
 * on wake, which is both what machine.deepsleep() promises and how the clock
 * tree gets rebuilt, since a Stop wake leaves the system running from HSI.
 * ms < 0 sleeps until an external interrupt, and with none configured that is
 * until NRST. */
MP_NORETURN void machine_sleep_deep(mp_int_t ms);

#endif // MICROPY_INCLUDED_CH32_MACHINE_SLEEP_H
