"""framebuf acceleration: the fast paths must be indistinguishable from the slow one.

Correctness here is differential, not absolute. Every case runs the same
drawing operation under all three backends -- generic C in extmod/modframebuf.c,
the port's word-wise fast paths, and the GPHA -- and demands byte-identical
buffers. That catches the failure modes this hardware actually has (a colour
register with a different field layout, a rectangle off by one line, a stride
gap applied to the wrong side) without anyone having to write out expected
pixels by hand.

Run on the target: python scripts/install_file.py ports/ch32/test_framebuf_gpha.py
"""

import framebuf
import gc
import uctypes

import ch32

# Buffers outside DTCM, which is what the GPHA path is gated on.
#
# This used to be a hardcoded 0x20140000, described as the unallocated tail of
# the shared region. Two things have since made that wrong: RAM_CODE grew from
# 240K to 320K, so 0x20140000 is now live .highcode and writing there corrupted
# the running firmware, and the genuinely unallocated tail past ETH_RAM is now
# the GC's second heap area (MICROPY_GC_SPLIT_HEAP). There is no address left
# to hardcode, so shared buffers come from the heap instead.
#
# gc_alloc searches area 1 (DTCM) first and only falls through to area 2 when
# area 1 cannot satisfy the request, so the only way to place an object outside
# DTCM is to leave area 1 with nothing free. _ballast holds what that costs;
# release_shared() drops it, without which the later DTCM cases have no room.
DTCM_END = 0x20100000
_ballast = []


def _fill_dtcm():
    """Occupy heap area 1 entirely, so later allocations land in area 2.

    A freshly collected area has one contiguous free run, so a single
    correctly sized allocation fills it; the loop is a binary search for that
    run's length. Filling rather than merely shrinking it matters because the
    buffers below go down to 640 bytes, and any free tail left in DTCM would
    swallow those -- the small cases would then silently test the DTCM path
    twice and never reach the GPHA at all.
    """
    if _ballast:
        return
    gc.collect()
    lo, hi = 0, 256 * 1024
    while lo < hi:
        mid = (lo + hi + 1) >> 1
        try:
            in_dtcm = uctypes.addressof(bytearray(mid)) < DTCM_END
        except MemoryError:
            in_dtcm = False
        gc.collect()
        if in_dtcm:
            lo = mid
        else:
            hi = mid - 1
    _ballast.append(bytearray(lo))


def shared_bytearray(n):
    """A bytearray outside DTCM, i.e. in the GC's second heap area."""
    _fill_dtcm()
    buf = bytearray(n)
    if uctypes.addressof(buf) < DTCM_END:
        raise MemoryError("could not place a %d byte buffer outside DTCM" % n)
    return buf


def release_shared():
    del _ballast[:]
    gc.collect()


MODES = (0, 1, 2)  # off / C fast paths / GPHA
passed = 0
failed = 0


def check(name, cond):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print("FAIL", name)


def run_one(fn, w, h, stride, where, mode):
    """Run fn(FrameBuffer) under one backend, return the resulting bytes."""
    ch32.framebuf_accel(mode)
    n = (stride * h) * 2
    if where == "dtcm":
        buf = bytearray(n)
    elif where == "shared":
        buf = shared_bytearray(n)
    else:
        # Deliberately misaligned by two bytes. The owning allocation stays
        # referenced through the whole call, so the view cannot outlive it.
        owner = shared_bytearray(n + 2)
        buf = uctypes.bytearray_at(uctypes.addressof(owner) + 2, n)
    for i in range(n):
        buf[i] = 0x5A
    fb = framebuf.FrameBuffer(buf, w, h, framebuf.RGB565, stride)
    fn(fb)
    return bytes(buf)


def same(name, fn, w, h, stride, where):
    """Demand that all three backends produce byte-identical results.

    Compared one at a time against the reference rather than collecting all
    three first: holding three copies of a 128x64 framebuffer plus the working
    buffers exhausts the DTCM heap.
    """
    gc.collect()
    ref = run_one(fn, w, h, stride, where, 0)
    for mode, label in ((1, " C==generic"), (2, " GPHA==generic")):
        gc.collect()
        check(name + label, run_one(fn, w, h, stride, where, mode) == ref)
    del ref
    ch32.framebuf_accel(2)
    gc.collect()


print("gpha_available", ch32.gpha_available())
print("default mode", ch32.framebuf_accel())

# --- fill and fill_rect, both regions, including sizes either side of the
# 1024-pixel GPHA threshold and strides wider than the drawn width.
for where in ("dtcm", "shared", "odd"):
    for w, h, stride in (
        (64, 32, 64),
        (64, 32, 80),
        (33, 17, 40),
        (128, 64, 128),
        (1, 40, 8),
        (40, 1, 48),
        (17, 61, 17),
    ):
        same(
            "fill %s %dx%d/%d" % (where, w, h, stride),
            lambda fb: fb.fill(0xF81F),
            w,
            h,
            stride,
            where,
        )
        same(
            "fill_rect %s %dx%d/%d" % (where, w, h, stride),
            lambda fb: fb.fill_rect(1, 1, w - 2 if w > 2 else 1, h - 2 if h > 2 else 1, 0x07E0),
            w,
            h,
            stride,
            where,
        )
    # Clipped and negative coordinates must clip identically.
    same(
        "fill_rect clip %s" % where,
        lambda fb: fb.fill_rect(-20, -10, 200, 200, 0x001F),
        64,
        48,
        64,
        where,
    )
    gc.collect()

# --- colour handling across every channel boundary. The word-wise fill packs
# two pixels into one 32-bit store, so a value that is wrong only in the high
# or low half would survive a single-colour test.
for col in (0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F, 0x8410, 0x1234, 0xABCD):
    same("colour 0x%04X" % col, lambda fb: fb.fill(col), 64, 32, 64, "shared")

# --- blit, including sub-rectangles, clipping and off-screen placement.
for where in ("dtcm", "shared"):
    # The DTCM cases need area 1 back, so this must run before each of them,
    # not once at the top.
    release_shared()
    n = 64 * 32 * 2
    if where == "dtcm":
        srcbuf = bytearray(n)
    else:
        srcbuf = shared_bytearray(n)
    src = framebuf.FrameBuffer(srcbuf, 64, 32, framebuf.RGB565)
    for y in range(32):
        for x in range(64):
            src.pixel(x, y, (x * 7 + y * 31) & 0xFFFF)

    for dx, dy in ((0, 0), (10, 5), (-8, -4), (60, 30), (-70, 0)):
        same(
            "blit %s at %d,%d" % (where, dx, dy),
            lambda fb: fb.blit(src, dx, dy),
            128,
            64,
            128,
            where,
        )
    # A destination stride wider than its width: the line gap must be applied
    # to the destination only.
    same("blit %s wide stride" % where, lambda fb: fb.blit(src, 3, 2), 100, 50, 128, where)
    gc.collect()

# --- key and palette blits must NOT take the accelerated path, since neither
# concept exists in the hardware. Equality across modes proves the hook
# declined rather than guessed.
palbuf = bytearray(4)
pal = framebuf.FrameBuffer(palbuf, 2, 1, framebuf.RGB565)
pal.pixel(0, 0, 0x1234)
pal.pixel(1, 0, 0x5678)
keybuf = bytearray(64 * 32 * 2)
keysrc = framebuf.FrameBuffer(keybuf, 64, 32, framebuf.RGB565)
keysrc.fill(0x07E0)
keysrc.fill_rect(0, 0, 20, 20, 0x0000)
same("blit with key", lambda fb: fb.blit(keysrc, 0, 0, 0x0000), 128, 64, 128, "shared")

gs8buf = bytearray(64 * 32)
gs8src = framebuf.FrameBuffer(gs8buf, 64, 32, framebuf.GS8)
for y in range(32):
    for x in range(64):
        gs8src.pixel(x, y, (x + y) & 1)
same("blit with palette", lambda fb: fb.blit(gs8src, 0, 0, -1, pal), 128, 64, 128, "shared")


# --- self-blit overlaps, where the generic path's increasing-order per-pixel
# walk is observable and neither memcpy nor the GPHA reproduces it.
def self_blit(fb):
    fb.fill_rect(0, 0, 30, 30, 0x1234)
    fb.blit(fb, 10, 10)


same("self blit overlap", self_blit, 64, 64, 64, "shared")

# --- formats the accelerator does not handle must be untouched by it.
for fmt, name in (
    (framebuf.MONO_VLSB, "MVLSB"),
    (framebuf.GS8, "GS8"),
    (framebuf.GS4_HMSB, "GS4"),
    (framebuf.MONO_HLSB, "MHLSB"),
):
    res = []
    for m in MODES:
        ch32.framebuf_accel(m)
        b = bytearray(64 * 32)
        fb = framebuf.FrameBuffer(b, 64, 32, fmt)
        fb.fill(1)
        fb.fill_rect(3, 3, 20, 10, 0)
        res.append(bytes(b))
        gc.collect()
    ch32.framebuf_accel(2)
    check("format " + name + " C==generic", res[1] == res[0])
    check("format " + name + " GPHA==generic", res[2] == res[0])

# --- the mode selector itself.
for m in MODES:
    ch32.framebuf_accel(m)
    check("mode %d round trip" % m, ch32.framebuf_accel() == m)
ch32.framebuf_accel(2)
try:
    ch32.framebuf_accel(3)
    check("bad mode rejected", False)
except ValueError:
    check("bad mode rejected", True)

# --- and finally: prove the GPHA was actually used. Every check above passes
# just as happily when the GPHA path declines every call and falls through to
# the C one, which is exactly what a clock-enable race made it do for one
# round of this test.
if ch32.gpha_available():
    check("GPHA path was exercised", ch32.gpha_ops() > 0)
    print("gpha_ops", ch32.gpha_ops())
else:
    print("GPHA absent on this die; only the C fallback was tested")

print("PASS", passed, "FAIL", failed)
print("RESULT", "OK" if failed == 0 else "FAILED")
