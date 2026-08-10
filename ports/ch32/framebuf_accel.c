/* Port hooks for extmod/modframebuf.c.
 *
 * Two layers sit behind these hooks, and which one runs is decided per call:
 *
 *   1. C fast paths -- word-wise fill and per-line memcpy. framebuf's generic
 *      code is a 16-bit store loop and a per-pixel walk through two function
 *      pointers, and on this part it is instruction-fetch bound rather than
 *      data bound, so this alone is worth about 4x on fill and 50x on blit.
 *
 *   2. The GPHA, when it is genuinely faster. It is not always: measured on
 *      64 KB, the core does 259 MB/s in DTCM against the GPHA's ~190, because
 *      DTCM is zero-wait at 400 MHz. Outside DTCM the core drops to 131 MB/s
 *      while the GPHA is unchanged -- it sits on the bus and does not care --
 *      so there the accelerator wins by about 1.5x. Hence the region test.
 *
 * Only RGB565 is a candidate: the GPHA speaks the direct and indexed colour
 * formats, so framebuf's MONO_* and GS* formats have nothing to gain from it,
 * and their generic implementations are left alone.
 */

#include <string.h>

#include "py/mphal.h"
#include "py/runtime.h"

#include "framebuf_accel.h"
#include "gpha.h"

// Mirrors the format numbering in extmod/modframebuf.c, which does not expose
// it in a header. Only this one value is needed.
#define FRAMEBUF_RGB565 (1)

/* DTCM is the one region where the CPU beats the GPHA, and it is where the GC
 * heap lives -- so a framebuffer built on a plain bytearray lands here and
 * should stay on the CPU. Buffers big enough to matter (a 320x240 RGB565
 * framebuffer is 150 KB) do not fit the DTCM heap anyway and end up in the
 * shared region, which is where the GPHA earns its place. */
#define DTCM_START (0x200C0000u)
#define DTCM_END   (0x20100000u)

/* Below this many pixels the GPHA's fixed cost -- a dozen register stores plus
 * the completion poll -- outweighs its bandwidth advantage. Measured on
 * hardware; see docs/hw/benchmarks.md. */
#define GPHA_MIN_PIXELS (1024)

static uint8_t accel_mode = FRAMEBUF_ACCEL_GPHA;

/* Counts transfers the GPHA actually performed. Without it a test can only
 * show that the backends agree, which they also do when the GPHA path is
 * silently declining every call and falling through to the C one -- exactly
 * what happened while the clock-enable race in gpha_available() was live. */
static uint32_t accel_gpha_ops;

void framebuf_accel_set_mode(uint8_t mode) {
    accel_mode = mode;
}

uint8_t framebuf_accel_get_mode(void) {
    return accel_mode;
}

uint32_t framebuf_accel_gpha_ops(void) {
    return accel_gpha_ops;
}

static inline bool in_dtcm(const void *p) {
    uintptr_t a = (uintptr_t)p;
    return a >= DTCM_START && a < DTCM_END;
}

// Byte range spanned by a w x h rectangle of 16-bit pixels at `base`.
static inline void rect_range(const void *buf, uint32_t stride, uint32_t x, uint32_t y,
    uint32_t w, uint32_t h, uintptr_t *lo, uintptr_t *hi) {
    const uint16_t *start = (const uint16_t *)buf + (size_t)y * stride + x;
    *lo = (uintptr_t)start;
    *hi = (uintptr_t)(start + (size_t)(h - 1) * stride + w);
}

static void fill_rgb565_c(void *buf, uint32_t stride, uint32_t x, uint32_t y,
    uint32_t w, uint32_t h, uint16_t col) {
    uint16_t *line = (uint16_t *)buf + (size_t)y * stride + x;
    uint32_t pair = ((uint32_t)col << 16) | col;

    while (h--) {
        uint16_t *p = line;
        uint32_t n = w;
        // Reach a 4-byte boundary so the bulk of the line moves 32 bits at a
        // time; an unaligned 32-bit store would trap or be split.
        if (n && ((uintptr_t)p & 2)) {
            *p++ = col;
            n--;
        }
        uint32_t *q = (uint32_t *)p;
        for (uint32_t k = n >> 1; k; k--) {
            *q++ = pair;
        }
        if (n & 1) {
            *(uint16_t *)q = col;
        }
        line += stride;
    }
}

static void blit_rgb565_c(void *dbuf, uint32_t dstride, uint32_t dx, uint32_t dy,
    const void *sbuf, uint32_t sstride, uint32_t sx, uint32_t sy,
    uint32_t w, uint32_t h) {
    uint16_t *d = (uint16_t *)dbuf + (size_t)dy * dstride + dx;
    const uint16_t *s = (const uint16_t *)sbuf + (size_t)sy * sstride + sx;

    while (h--) {
        memcpy(d, s, (size_t)w * 2);
        d += dstride;
        s += sstride;
    }
}

bool mp_framebuf_accel_fill_rect(void *buf, unsigned int stride, unsigned int format,
    unsigned int x, unsigned int y, unsigned int w, unsigned int h, uint32_t col) {

    if (accel_mode == FRAMEBUF_ACCEL_OFF || format != FRAMEBUF_RGB565 || w == 0 || h == 0) {
        return false;
    }

    /* No GPHA path here, deliberately. It fills at 196 MB/s while a word-wise
     * CPU fill manages ~385 MB/s even into the shared region, because writes
     * post and buffer -- it is reads that pay the HCLK penalty, which is why
     * blit below is a different story. Measured at 256x128 in shared SRAM:
     * 170 us on the core against 336 us on the GPHA. Routing fills through the
     * accelerator would be a 2x regression. See docs/hw/benchmarks.md. */
    fill_rgb565_c(buf, stride, x, y, w, h, (uint16_t)col);
    return true;
}

bool mp_framebuf_accel_blit(void *dbuf, unsigned int dstride, unsigned int dformat,
    unsigned int dx, unsigned int dy,
    const void *sbuf, unsigned int sstride, unsigned int sformat,
    unsigned int sx, unsigned int sy, unsigned int w, unsigned int h) {

    if (accel_mode == FRAMEBUF_ACCEL_OFF || w == 0 || h == 0
        || dformat != FRAMEBUF_RGB565 || sformat != FRAMEBUF_RGB565) {
        return false;
    }

    /* Overlapping copies go back to the generic path. Its per-pixel walk in
     * increasing x and y has defined behaviour that neither memcpy nor the
     * GPHA reproduces, and blitting a framebuffer onto itself is the one case
     * where that difference is observable. */
    uintptr_t dlo, dhi, slo, shi;
    rect_range(dbuf, dstride, dx, dy, w, h, &dlo, &dhi);
    rect_range(sbuf, sstride, sx, sy, w, h, &slo, &shi);
    if (dlo < shi && slo < dhi) {
        return false;
    }

    uint16_t *d = (uint16_t *)dbuf + (size_t)dy * dstride + dx;
    const uint16_t *s = (const uint16_t *)sbuf + (size_t)sy * sstride + sx;

    if (accel_mode >= FRAMEBUF_ACCEL_GPHA
        && (size_t)w * h >= GPHA_MIN_PIXELS
        && !(in_dtcm(d) && in_dtcm(s))
        && gpha_copy_rgb565(d, dstride, s, sstride, w, h)) {
        accel_gpha_ops++;
        return true;
    }

    blit_rgb565_c(dbuf, dstride, dx, dy, sbuf, sstride, sx, sy, w, h);
    return true;
}
