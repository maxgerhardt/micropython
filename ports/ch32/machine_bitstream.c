/* machine.bitstream() for the CH32H417 -- the timed pin driver that
 * neopixel.py and anything else WS2812-shaped is built on.
 *
 * Bit timing comes from SysTick0->CNT, which counts HCLK. Two things about
 * that counter shape this file:
 *
 * 1. It is *modular*, not free-running. mphalport.c runs it with auto-reload
 *    and a 1 ms compare, so it counts 0..(HCLK/1000 - 1) and starts again --
 *    plain `now - start` goes badly wrong across the reload, in the direction
 *    that ends a bit early. Every elapsed-time comparison here therefore wraps
 *    explicitly. A whole bit is about 125 ticks against a period of 100000, so
 *    at most one reload can fall inside one bit and one correction is enough.
 *
 * 2. It ticks at HCLK (100 MHz), not at the V5F's 400 MHz core clock, so a
 *    tick is 10 ns. WS2812 wants 350 ns and 700 ns highs, i.e. 35 and 70
 *    ticks, so the resolution is ample -- but the conversion has to use the
 *    SysTick rate and not SystemCoreClock, which is four times larger. It is
 *    recovered from the compare register rather than assumed, so a change to
 *    the clock tree cannot silently detune this.
 *
 * Interrupts are masked for the whole transfer, as on every other port: an
 * interrupt landing inside a bit's high phase stretches a zero into something
 * a WS2812 reads as a one. That stalls the millisecond clock, which for a bit
 * banger that runs for microseconds -- dht_readinto(), onewire -- does not
 * matter, but a 400 LED write takes 13 ms and would lose 12 of them. So the
 * reloads are counted here and handed back before the unmask; the loop has to
 * watch the counter anyway, and one compare per bit is what it costs.
 */

#include "ch32h417.h"

#include "py/mpconfig.h"
#include "py/mphal.h"

#include "mphalport.h"

#if MICROPY_PY_MACHINE_BITSTREAM

/* Ticks the loop spends on itself between sampling CNT and the pin moving.
 *
 * The measured figure is nearer 13: 400 WS2812 LEDs come out 7.5% over the
 * nominal 12000 us. This deliberately under-compensates, because the two
 * errors are not symmetric. Running the *period* long is free -- WS2812 only
 * cares that the gap stays under the ~50 us that latches the strip -- while
 * running the *high* time short is what turns a one into a zero, and T0H has
 * only about 150 ns of margin below the 350 ns that neopixel.py asks for.
 * Subtracting the true overhead would spend all of it. */
#define BITSTREAM_TICKS_OVERHEAD (4)

static inline uint32_t bitstream_elapsed(uint32_t start, uint32_t period) {
    uint32_t now = SysTick0->CNT;
    return now >= start ? now - start : now + period - start;
}

void machine_bitstream_high_low(mp_hal_pin_obj_t pin, uint32_t *timing_ns,
    const uint8_t *buf, size_t len) {
    /* CMP holds the reload minus one, so this is the tick count of a whole
     * millisecond -- and the modulus of CNT. */
    uint32_t period = SysTick0->CMP + 1u;
    uint32_t ticks_per_us = period / 1000u;

    /* Convert to ticks in place, as [high_0, period_0, high_1, period_1]: the
     * inner loop compares both phases against the same start, so the low time
     * is easier to use as a total. 64-bit because 800 ns * 100 already needs
     * more than 16 bits and a caller may ask for far longer. */
    uint32_t t[4];
    for (size_t i = 0; i < 4; ++i) {
        t[i] = (uint32_t)(((uint64_t)timing_ns[i] * ticks_per_us) / 1000u);
        if (t[i] > BITSTREAM_TICKS_OVERHEAD) {
            t[i] -= BITSTREAM_TICKS_OVERHEAD;
        }
        if (i % 2 == 1) {
            t[i] += t[i - 1];
        }
    }

    uint32_t irq_state = mp_hal_quiet_timing_enter();

    /* Reloads seen, so the millisecond clock can be corrected afterwards. A
     * bit is ~125 ticks against a period of 100000, so consecutive bit starts
     * can straddle at most one reload and "went backwards" counts it exactly. */
    uint32_t reloads = 0;
    uint32_t previous = SysTick0->CNT;

    for (size_t i = 0; i < len; ++i) {
        uint8_t b = buf[i];
        for (size_t j = 0; j < 8; ++j) {
            /* Index 0 or 2 from the top bit, so the two timings are picked
             * without a branch. */
            const uint32_t *bit = &t[(b >> 6) & 2];
            uint32_t start = SysTick0->CNT;
            mp_hal_pin_high(pin);
            if (start < previous) {
                ++reloads;
            }
            previous = start;
            while (bitstream_elapsed(start, period) < bit[0]) {
            }
            mp_hal_pin_low(pin);
            b <<= 1;
            while (bitstream_elapsed(start, period) < bit[1]) {
            }
        }
    }

    /* Minus one: the handler is already pending for the most recent reload and
     * will count that one itself the moment interrupts come back. */
    if (reloads > 1) {
        mp_hal_systick_recover_ms(reloads - 1);
    }

    mp_hal_quiet_timing_exit(irq_state);
}

#endif // MICROPY_PY_MACHINE_BITSTREAM
