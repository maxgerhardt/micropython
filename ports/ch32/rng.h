#ifndef MICROPY_INCLUDED_CH32_RNG_H
#define MICROPY_INCLUDED_CH32_RNG_H

#include <stdint.h>

/* Hardware TRNG, whitened. Raw words from this peripheral carry only about 10
 * bits of entropy each, so several are folded into every value returned -- see
 * the measurements at the top of rng.c before assuming otherwise.
 *
 * Both raise OSError if the peripheral reports a clock error or stops
 * producing values, rather than returning a number that cannot be vouched for. */
uint32_t ch32_rng_u32(void);
uint64_t ch32_rng_u64(void);

#endif // MICROPY_INCLUDED_CH32_RNG_H
