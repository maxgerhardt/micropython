#ifndef MICROPY_INCLUDED_CH32_MPHALPORT_H
#define MICROPY_INCLUDED_CH32_MPHALPORT_H

#include <stdint.h>
#include <stddef.h>
#include "py/mpconfig.h"
#include "shared/runtime/interrupt_char.h"

// This header is included from py/mphal.h, which already declares the standard
// mp_hal_* API. Only port-specific additions belong here.

// Provided by the SDK's system_ch32h417.c.
extern uint32_t SystemCoreClock;

void mp_hal_init(void);

#endif // MICROPY_INCLUDED_CH32_MPHALPORT_H
