#ifndef MICROPY_INCLUDED_CH32_MPHALPORT_H
#define MICROPY_INCLUDED_CH32_MPHALPORT_H

#include <stdint.h>
#include <stddef.h>
#include "py/mpconfig.h"
#include "py/ringbuf.h"
#include "shared/runtime/interrupt_char.h"

/* Every console feeds one buffer: the USART1 ISR and, when enumerated, the USB
 * CDC layer in shared/tinyusb. mp_hal_stdin_rx_chr drains it, so input is
 * accepted from whichever console the user is actually on. */
extern ringbuf_t stdin_ringbuf;

static inline void mp_hal_wake_main_task_from_isr(void) {
    /* Nothing to wake: this port has no scheduler thread and the REPL polls. */
}

// This header is included from py/mphal.h, which already declares the standard
// mp_hal_* API. Only port-specific additions belong here.

// Provided by the SDK's system_ch32h417.c.
extern uint32_t SystemCoreClock;

void mp_hal_init(void);

#endif // MICROPY_INCLUDED_CH32_MPHALPORT_H
