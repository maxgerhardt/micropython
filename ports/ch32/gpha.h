#ifndef MICROPY_INCLUDED_CH32_GPHA_H
#define MICROPY_INCLUDED_CH32_GPHA_H

#include <stdbool.h>
#include <stdint.h>

// Largest rectangle a single GPHA transfer can describe: NLR packs PL[13:0] at
// bit 16 and NL[15:0] at bit 0, and the line-offset registers are 14 bits.
#define GPHA_MAX_WIDTH  (16383)
#define GPHA_MAX_HEIGHT (65535)
#define GPHA_MAX_OFFSET (16383)

// True if this die actually has a GPHA. Probed once on first call: the
// datasheet makes the block a per-lot fuse ("for products with lot number 5
// bit 0, GPHA, Ethernet, SerDes, CAN functions are not provided"), so its
// presence cannot be inferred from the part number.
bool gpha_available(void);

// Copy w x h RGB565 pixels from src to dst, each with its own stride in pixels.
// Source and destination must not overlap.
bool gpha_copy_rgb565(void *dst, uint32_t dstride, const void *src, uint32_t sstride,
    uint32_t w, uint32_t h);

#endif // MICROPY_INCLUDED_CH32_GPHA_H
