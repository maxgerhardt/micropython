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

#endif // MICROPY_INCLUDED_CH32_MACHINE_RTC_H
