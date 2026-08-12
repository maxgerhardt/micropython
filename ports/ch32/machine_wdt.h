#ifndef MICROPY_INCLUDED_CH32_MACHINE_WDT_H
#define MICROPY_INCLUDED_CH32_MACHINE_WDT_H

#include "py/obj.h"

/* machine.WDT, both watchdogs, selected by id: 0 is the independent one and
 * 1 is the window one. See machine_wdt.c for which to reach for. */
extern const mp_obj_type_t machine_wdt_type;

/* machine.reset_cause() values. Their numbering is a port's own business --
 * code compares against machine.PWRON_RESET and friends, never the integer. */
#define CH32_RESET_PWRON (1)
#define CH32_RESET_HARD  (2)
#define CH32_RESET_WDT   (3)
#define CH32_RESET_SOFT  (4)
#define CH32_RESET_DEEPSLEEP (5)

/* Read and clear the RCC reset flags. Must run early in main(), before
 * anything else can clear them, because they accumulate across resets. */
void machine_wdt_reset_cause_init(void);

/* Start the IWDG for `timeout_ms` without creating a WDT object, so
 * machine.deepsleep() can use an IWDG reset as its wake source -- reference
 * manual table 2-1 lists that as one of only three ways out of Stop mode.
 * False if the timeout is beyond the IWDG's ~26 s ceiling. */
bool machine_wdt_start_raw(uint32_t timeout_ms);

/* True once anything has started the IWDG. deepsleep must not steal a watchdog
 * the application is already relying on: the IWDG cannot be stopped or
 * retimed downwards once running. */
bool machine_wdt_is_running(void);

mp_int_t machine_wdt_reset_cause(void);

/* Raw RCC_RSTSCKR as it stood at boot, before the flags were cleared. Exposed
 * because the cooked cause above collapses several distinct causes into SOFT. */
uint32_t machine_wdt_reset_flags(void);

#endif // MICROPY_INCLUDED_CH32_MACHINE_WDT_H
