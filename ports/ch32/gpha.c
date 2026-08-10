/* Graphics Processing Hardware Accelerator (GPHA).
 *
 * A DMA2D/Chrom-ART clone. Only the rectangle copy is exposed, because that is
 * the only thing it does faster than the core: its register-to-memory fill
 * runs at 196 MB/s against ~385 MB/s for a word-wise CPU fill, so framebuf
 * fills stay on the core. See docs/hw/benchmarks.md for the measurements and
 * docs/hw/ch32h417-notes.md for how to bring the fill back if a framebuffer
 * ever lives somewhere the core reaches slowly, such as external SDRAM -- note
 * especially that OCOLR takes ARGB8888 fields rather than a raw RGB565 word.
 *
 * This driver knows nothing about framebuf; framebuf_accel.c owns the policy
 * of when calling it is a win.
 *
 * Registers are driven directly rather than through the SDK's GPHA_Init(),
 * which read-modify-writes a dozen registers for what is a handful of stores
 * on a path that runs per drawing operation.
 *
 * Three things about this peripheral cost real time to discover, and all three
 * are load-bearing below. See docs/hw/ch32h417-notes.md.
 */

#include "ch32h417.h"
#include "py/mphal.h"
#include "gpha.h"

/* CTLR.MODE. MODE=0 -- plain memory-to-memory, which STM32's DMA2D defines and
 * implements -- NEVER COMPLETES on this part: START stays set and ISR reads
 * 0x0, not even an error flag, from every source region (DTCM, ITCM, flash,
 * shared SRAM). MODE=1 does the same job and works from all of them, so the
 * copy path uses the pixel-format-converting mode even though it converts
 * RGB565 to RGB565. Do not "optimise" this to MODE=0. */
#define GPHA_MODE_M2M_PFC (0x00010000)

#define GPHA_CM_RGB565    (2)
#define GPHA_ISR_ALL      (0x0000003F)

// Every flag except transfer-complete and the watermark is an error.
#define GPHA_ISR_ERRORS   (GPHA_ISR_CAEIF | GPHA_ISR_CEIF)

static int8_t gpha_present = -1;  // -1 unprobed, 0 absent, 1 present

bool gpha_available(void) {
    if (gpha_present >= 0) {
        return gpha_present != 0;
    }

    RCC_HB2PeriphClockCmd(RCC_HB2Periph_GPHA, ENABLE);

    /* The clock does not arrive on the same cycle it is asked for, and the
     * enable is a read-modify-write so nothing reads the register afterwards
     * to push the store out. Without this wait the first access to GPHA
     * registers is dropped and the probe below concludes the block is absent
     * -- which it did, on a die where the peripheral demonstrably works when
     * driven from the REPL, where milliseconds pass between the two writes. */
    (void)RCC->HB2PCENR;
    mp_hal_delay_us(10);

    /* Probe by writing a register that has no side effects and reading it
     * back. A die without the block leaves the bus value behind instead.
     * Two different patterns, so a floating bus that happens to match one of
     * them cannot pass. */
    GPHA->OMAR = 0xA5A5A5A4;
    bool ok = (GPHA->OMAR == 0xA5A5A5A4);
    GPHA->OMAR = 0x5A5A5A58;
    ok = ok && (GPHA->OMAR == 0x5A5A5A58);
    GPHA->OMAR = 0;

    gpha_present = ok ? 1 : 0;
    if (!ok) {
        RCC_HB2PeriphClockCmd(RCC_HB2Periph_GPHA, DISABLE);
    }
    return ok;
}

/* Wait for the current transfer, bounded.
 *
 * The bound is not defensive programming, it is required: a GPHA that stalls
 * leaves START set forever with no error flag, and an unbounded wait would
 * hang the board with nothing to show for it. On timeout the transfer is
 * aborted and the peripheral reset, so a single bad call cannot poison the
 * next one, and false sends the caller to the CPU path.
 *
 * The budget is generous -- roughly 25x the measured 193 MB/s -- because it
 * exists to catch a stall, not to police performance. */
static bool gpha_wait(uint32_t pixels) {
    uint32_t budget_us = 1000 + pixels / 8;
    mp_uint_t start = mp_hal_ticks_us();

    while (GPHA->CTLR & GPHA_CTLR_START) {
        if ((mp_uint_t)(mp_hal_ticks_us() - start) > budget_us) {
            GPHA->CTLR |= GPHA_CTLR_ABORT;
            RCC_HB2PeriphResetCmd(RCC_HB2Periph_GPHA, ENABLE);
            RCC_HB2PeriphResetCmd(RCC_HB2Periph_GPHA, DISABLE);
            return false;
        }
    }

    uint32_t isr = GPHA->ISR;
    GPHA->IFCR = GPHA_ISR_ALL;
    return (isr & GPHA_ISR_ERRORS) == 0;
}

// Common geometry limits. Offsets are the gap between lines, in pixels.
static bool gpha_geometry_ok(uint32_t w, uint32_t h, uint32_t off1, uint32_t off2) {
    return w > 0 && h > 0
           && w <= GPHA_MAX_WIDTH && h <= GPHA_MAX_HEIGHT
           && off1 <= GPHA_MAX_OFFSET && off2 <= GPHA_MAX_OFFSET;
}

bool gpha_copy_rgb565(void *dst, uint32_t dstride, const void *src, uint32_t sstride,
    uint32_t w, uint32_t h) {
    if (!gpha_available() || dstride < w || sstride < w) {
        return false;
    }
    if (!gpha_geometry_ok(w, h, dstride - w, sstride - w)) {
        return false;
    }
    if (((uintptr_t)dst | (uintptr_t)src) & 1) {
        return false;
    }

    GPHA->IFCR = GPHA_ISR_ALL;
    GPHA->FGPFCCR = GPHA_CM_RGB565;
    GPHA->FGMAR = (uint32_t)src;
    GPHA->FGOR = sstride - w;
    GPHA->OPFCCR = GPHA_CM_RGB565;
    GPHA->OMAR = (uint32_t)dst;
    GPHA->OOR = dstride - w;
    GPHA->NLR = (w << 16) | h;
    GPHA->CTLR = GPHA_MODE_M2M_PFC;
    GPHA->CTLR = GPHA_MODE_M2M_PFC | GPHA_CTLR_START;

    return gpha_wait(w * h);
}
