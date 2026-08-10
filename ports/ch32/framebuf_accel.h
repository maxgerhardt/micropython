#ifndef MICROPY_INCLUDED_CH32_FRAMEBUF_ACCEL_H
#define MICROPY_INCLUDED_CH32_FRAMEBUF_ACCEL_H

#include <stdbool.h>
#include <stdint.h>

/* Which backend framebuf's accelerated paths use. Selectable at runtime purely
 * so one firmware image can produce all three sets of benchmark numbers under
 * identical conditions -- comparing across builds would leave code layout free
 * to move underneath the measurement. */
enum {
    FRAMEBUF_ACCEL_OFF = 0,   // generic code in extmod/modframebuf.c
    FRAMEBUF_ACCEL_C = 1,     // word-wise fill and per-line memcpy
    FRAMEBUF_ACCEL_GPHA = 2,  // as above, plus the GPHA where it wins
};

void framebuf_accel_set_mode(uint8_t mode);
uint8_t framebuf_accel_get_mode(void);

// Number of operations the GPHA has actually performed, so a test can tell
// "the backends agree" from "the GPHA path never ran".
uint32_t framebuf_accel_gpha_ops(void);

#endif // MICROPY_INCLUDED_CH32_FRAMEBUF_ACCEL_H
