#ifndef MICROPY_INCLUDED_CH32_MACHINE_RTC_H
#define MICROPY_INCLUDED_CH32_MACHINE_RTC_H

#include "py/obj.h"

extern const mp_obj_type_t machine_rtc_type;

/* Bring the clock up at boot, keeping whatever time is already running. */
void machine_rtc_init_boot(void);

/* Seconds since 2000-01-01 and the microseconds within the current second, read
 * as one coherent instant. Returns false if the clock is not running, in which
 * case neither output is touched. */
bool machine_rtc_get(uint32_t *seconds, uint32_t *microseconds);

/* Arm the alarm `seconds` from now, so it can end a machine.deepsleep().
 * False if no oscillator is running. Granularity is one whole second: the
 * alarm is a bare comparator against the same 32-bit second counter, and this
 * RTC has nothing finer. machine_sleep.c uses LPTIM below the crossover. */
bool machine_rtc_alarm_in(uint32_t seconds);

/* Disarm and clear a pending alarm. Safe to call when none was armed. */
void machine_rtc_alarm_clear(void);

#endif // MICROPY_INCLUDED_CH32_MACHINE_RTC_H
