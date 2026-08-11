/* Hardware true random number generator.
 *
 * Backs os.urandom() and the seed for the `random` module's PRNG.
 *
 * **Raw words from this peripheral are not uniformly distributed and must not
 * be handed out directly.** Measured on hardware: reading RNG_DR repeatedly
 * gives about 1400 distinct values in 4000 reads, and 421 in 600, where a
 * uniform 32-bit source would give essentially all-distinct. The unique count
 * saturates rather than growing linearly, so this is a limited value pool --
 * roughly 10 to 11 bits of entropy per word -- and not merely reading faster
 * than the generator refreshes. Every bit position does vary, spacing the
 * reads out to 1 ms does not help, a 1 second warm-up does not help, and it is
 * the same whether RNGSRC selects SYSCLK or PLL_CLK. The chip's own error
 * flags stay clear throughout (SR = 0x01: data ready, no seed or clock error),
 * so it does not consider anything to be wrong.
 *
 * That is normal for a raw noise source and is why entropy sources are fed
 * through a conditioner. mbedtls, for instance, accumulates many polls into a
 * hashed pool; the same CH32H417 RNG works fine there for exactly that reason.
 * Here the conditioning is explicit: CH32_RNG_MIX raw words are folded into
 * each word handed out. Measured after mixing just two, 600 of 600 draws are
 * distinct; four are used for margin, since 600 samples cannot tell 2^32 from
 * 2^20 and the raw reads are cheap.
 *
 * This is a whitened hardware entropy source suitable for seeding and for
 * os.urandom(). It is not claimed to be cryptographically strong, and the
 * measurements above are the reason to be careful about assuming so.
 */

#include <string.h>

#include "ch32h417.h"
#include "py/mphal.h"
#include "py/runtime.h"

#include "rng.h"

/* Raw words folded into each value handed out. See the file comment. */
#define CH32_RNG_MIX (4)

/* A word needs a fixed number of RNG clocks. The bound exists so a peripheral
 * that never asserts data-ready raises instead of hanging the interpreter. */
#define CH32_RNG_TIMEOUT_MS (10)

static bool rng_started;

static void rng_start(void) {
    if (rng_started) {
        return;
    }
    RCC_HBPeriphClockCmd(RCC_HBPeriph_RNG, ENABLE);
    /* Read back before touching the peripheral: the clock enable is a
     * read-modify-write, so nothing pushes the store out and the first access
     * can otherwise be dropped. */
    (void)RCC->HBPCENR;
    /* SYSCLK, matching what the vendor examples and a known-good mbedtls
     * entropy driver for this part use. Selecting PLL_CLK instead measured
     * identically, so this is not the knob that matters. */
    RCC_RNGCLKConfig(RCC_RNGCLKSource_SYSCLK);
    RNG_Cmd(ENABLE);
    rng_started = true;
}

// One raw word, with the peripheral's own error conditions handled.
static uint32_t rng_raw(void) {
    mp_uint_t start = mp_hal_ticks_ms();
    for (;;) {
        if (RNG_GetFlagStatus(RNG_FLAG_SECS) != RESET) {
            /* Seed error: the noise source failed its own checks and RNG_DR
             * must not be used. Recovery is to clear the flag and restart the
             * generator, per RM 30.2.2. */
            RNG_ClearFlag(RNG_FLAG_SECS);
            RNG_Cmd(DISABLE);
            RNG_Cmd(ENABLE);
        }
        if (RNG_GetFlagStatus(RNG_FLAG_CECS) != RESET) {
            /* The RNG clock is out of range. Nothing here can fix that, and
             * continuing would hand out numbers of unknown quality. */
            RNG_ClearFlag(RNG_FLAG_CECS);
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("RNG clock error"));
        }
        if (RNG_GetFlagStatus(RNG_FLAG_DRDY) != RESET) {
            return RNG_GetRandomNumber();
        }
        if ((mp_uint_t)(mp_hal_ticks_ms() - start) > CH32_RNG_TIMEOUT_MS) {
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("RNG timeout"));
        }
    }
}

/* Entropy pool. Persistent on purpose: folding only the last few raw words
 * into each output leaves successive outputs as correlated as the raw reads
 * are, which measured as about 18 effective bits -- 6 collisions in 2000
 * draws, where a uniform 32-bit source gives 0.0005. Carrying the pool across
 * calls means every output depends on every raw word ever read, so outputs
 * decorrelate even though the source underneath them does not. This is the
 * same shape as any entropy-pool design, just small. */
static uint64_t rng_pool;

uint32_t ch32_rng_u32(void) {
    rng_start();

    for (int i = 0; i < CH32_RNG_MIX; i++) {
        rng_pool ^= rng_raw();
        rng_pool *= 0x9E3779B97F4A7C15ull;
        rng_pool = (rng_pool << 31) | (rng_pool >> 33);
    }

    /* splitmix64's finaliser, so what is handed out is an avalanche of the
     * pool rather than the pool itself: entropy sitting in a few bits of the
     * raw words ends up spread across all 32 output bits, and the pool state
     * is not directly exposed. */
    uint64_t z = rng_pool;
    z ^= z >> 30;
    z *= 0xBF58476D1CE4E5B9ull;
    z ^= z >> 27;
    z *= 0x94D049BB133111EBull;
    z ^= z >> 31;
    return (uint32_t)z;
}

uint64_t ch32_rng_u64(void) {
    /* Two independently mixed words, not one doubled: random.seed() consumes
     * the whole 64 bits and reusing the top half would halve what the PRNG
     * actually starts from. */
    return ((uint64_t)ch32_rng_u32() << 32) | ch32_rng_u32();
}

void mp_hal_get_random(size_t n, void *buf) {
    uint8_t *dest = buf;
    while (n) {
        uint32_t word = ch32_rng_u32();
        size_t take = n < sizeof(word) ? n : sizeof(word);
        /* Copied rather than cast: the destination is a bytes object's buffer
         * and carries no alignment guarantee. */
        memcpy(dest, &word, take);
        dest += take;
        n -= take;
    }
}
