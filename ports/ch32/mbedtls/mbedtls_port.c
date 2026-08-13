/* mbedtls's two hooks into this port: entropy and a wall clock. */

#include <string.h>

#include "py/mpconfig.h"
#include "py/mphal.h"

#if MICROPY_SSL_MBEDTLS

#include "mbedtls_config_port.h"

#include "mbedtls/platform_time.h"

#include "machine_rtc.h"

#include "shared/timeutils/timeutils.h"

/* Entropy source. MBEDTLS_ENTROPY_HARDWARE_ALT and MBEDTLS_NO_PLATFORM_ENTROPY
 * are both set for bare-metal builds, so this is the only thing standing
 * between TLS and predictable keys.
 *
 * It goes through ch32_rng_u32() rather than reading the RNG data register
 * directly, and that matters more than it looks. Raw, this peripheral yields
 * only about ten bits of entropy per 32-bit word -- measured, 1364 distinct
 * values in 4000 reads -- and its DRDY flag is stuck at 1, so it is not a
 * data-ready signal and code that waits on it happily returns the previous
 * word again. rng.c already handles both; see the notes there. */
/* ...and it is NOT implemented here. extmod/mbedtls/mbedtls_alt.c already
 * defines mbedtls_hardware_poll() and routes it to mp_hal_get_random(), which
 * this port backs with exactly that conditioned TRNG. Defining it again is a
 * duplicate-symbol link error, and the shared one is already right. */

/* Wall clock for certificate validity.
 *
 * The RTC counts from 2000-01-01 and, unless something has set it, from the
 * moment power was applied -- so out of the box this reports a time in the
 * distant past and every certificate looks "not yet valid". That is a real
 * limitation rather than a bug to paper over: set machine.RTC() from the
 * network or by hand before verifying certificates, or use
 * ssl.CERT_NONE and accept what that means.
 *
 * Returning 0 when the clock is not running is deliberate; mbedtls then treats
 * dates as unverifiable rather than silently accepting an expired chain. */
/* Monotonic milliseconds, for mbedtls's interval timers. Deliberately not
 * derived from the RTC: this must not jump when the wall clock is set. */
mbedtls_ms_time_t mbedtls_ms_time(void) {
    return (mbedtls_ms_time_t)mp_hal_ticks_ms();
}

time_t ch32_mbedtls_time(time_t *timer) {
    uint32_t seconds = 0;
    uint32_t microseconds;

    if (!machine_rtc_get(&seconds, &microseconds)) {
        seconds = 0;
    }

    /* The RTC's epoch is 2000-01-01; mbedtls wants the Unix epoch. */
    time_t t = (time_t)seconds + (time_t)TIMEUTILS_SECONDS_1970_TO_2000;

    if (timer != NULL) {
        *timer = t;
    }
    return t;
}

#endif // MICROPY_SSL_MBEDTLS
