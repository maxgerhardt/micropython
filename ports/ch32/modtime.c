/* The two hooks extmod/modtime.c needs from the port, both answered by the RTC.
 *
 * The counter holds seconds since 2000-01-01, which is this port's epoch, so
 * these are pass-throughs rather than conversions. See machine_rtc.c for why
 * that epoch and not the Unix one, and why the result is good until 2136.
 *
 * If no oscillator started at boot the RTC reports nothing and these fall back
 * to the uptime that mp_hal_time_ns() returns, so the calls still work; the
 * clock simply reads as though the board booted at midnight on 2000-01-01. */
#include "py/obj.h"
#include "py/mphal.h"
#include "shared/timeutils/timeutils.h"

static void mp_time_localtime_get(timeutils_struct_time_t *tm) {
    timeutils_seconds_since_epoch_to_struct_time(
        (mp_uint_t)(mp_hal_time_ns() / 1000000000ull), tm);
}

static mp_obj_t mp_time_time_get(void) {
    return mp_obj_new_int_from_ull(mp_hal_time_ns() / 1000000000ull);
}
