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

/* Read and clear the RCC reset flags. Must run early in main(), before
 * anything else can clear them, because they accumulate across resets. */
void machine_wdt_reset_cause_init(void);

mp_int_t machine_wdt_reset_cause(void);

#endif // MICROPY_INCLUDED_CH32_MACHINE_WDT_H
