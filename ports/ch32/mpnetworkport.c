/* lwip's connection to this port's time base and main loop.
 *
 * lwip runs in NO_SYS mode: there is no thread to own it, so something has to
 * call sys_check_timeouts() regularly or every retransmit, DHCP renewal and
 * ARP expiry stops happening. That "something" is the same hook that already
 * pumps USB, which is reached both from the REPL's idle wait and from the VM
 * hook inside a long-running Python loop.
 */

#include "py/mphal.h"
#include "py/runtime.h"

#if MICROPY_PY_LWIP

#include "lwip/timeouts.h"

#include "mpnetworkport.h"

#if MICROPY_PY_NETWORK_LAN
#include "eth.h"
#endif

/* lwip's clock. It only requires a millisecond counter that does not go
 * backwards, which is exactly what SysTick gives us. */
uint32_t sys_now(void) {
    return mp_hal_ticks_ms();
}

/* Rate-limit to once per millisecond.
 *
 * The hook this runs from fires every 256 bytecodes, which at 400 MHz is far
 * more often than lwip's timer list can have anything new to do.
 * sys_check_timeouts() walks that list on every call, so calling it
 * unthrottled turns a Python loop into a timer-list scan -- pure overhead with
 * no change in behaviour, since lwip's own resolution is milliseconds. */
void mp_network_lwip_poll(void) {
    static uint32_t last_ms;
    static bool in_poll;

    /* Same reasoning as the guard in eth_rx_process(): everything below can
     * re-enter the interpreter, and this function is reached from the VM hook,
     * so without this it can run inside itself. sys_check_timeouts() in
     * particular must not be re-entered -- it walks a list it is mutating. */
    if (in_poll) {
        return;
    }
    in_poll = true;

    /* Received frames are handled on every call, not once a millisecond.
     * Twelve descriptors hold about 1.5 ms of a saturated 100 Mbit link, so
     * rate-limiting this to the same 1 ms as the timer sweep would leave
     * almost no margin for a hook that arrives late. It costs one flag read
     * when there is nothing to do. */
    #if MICROPY_PY_NETWORK_LAN
    eth_lwip_lock();
    eth_rx_process();
    eth_lwip_unlock();
    #endif

    /* The timer sweep and the link check are the parts that only need doing
     * once a millisecond; the receive drain above runs every time.
     *
     * Note this is not an early return -- in_poll has to be cleared on the way
     * out of every path, or the first throttled call would disable the poll
     * permanently. */
    uint32_t now = mp_hal_ticks_ms();
    if (now != last_ms) {
        last_ms = now;

        /* Masks the Ethernet interrupt across the two calls that walk lwip's
         * lists. Neither can raise, so the unlock cannot be skipped by an
         * exception unwinding past it -- which is exactly why
         * MICROPY_PY_LWIP_ENTER is left undefined. See mpconfigport.h. */
        eth_lwip_lock();
        sys_check_timeouts();
        #if MICROPY_PY_NETWORK_LAN
        eth_poll();
        #endif
        eth_lwip_unlock();
    }

    in_poll = false;
}

#endif // MICROPY_PY_LWIP
