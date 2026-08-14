# The split GC heap: DTCM plus the tail of the shared region.
#
# MICROPY_GC_SPLIT_HEAP is on and main() calls gc_add() for .heap2, so the heap
# spans two disjoint areas. Nothing about that is visible from Python except
# through the addresses objects land at, which is what this file checks.
#
# Run on the target: python scripts/install_file.py ports/ch32/test_heap.py
import gc
import uctypes

# The DTCM window ends here; everything above is the shared region.
DTCM_START = 0x200C0000
DTCM_END = 0x20100000
HEAP2_START = 0x2015C000
HEAP2_END = 0x20180000

passed = 0
failed = 0


def check(name, cond):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print("FAIL", name)


gc.collect()
free = gc.mem_free()
total = free + gc.mem_alloc()

# Both areas are counted. Area 1 alone is about 162K and area 2 adds 144K, less
# each area's allocation tables -- so anything near 300K proves the second area
# was added, and anything near 160K proves gc_add() never ran.
print("heap total", total, "free", free)
check("heap spans both areas", total > 280 * 1024)

# A small allocation on a collected heap goes to area 1: gc_alloc() searches
# from the front of the area list and area 1 has room.
small = bytearray(64)
addr = uctypes.addressof(small)
check("small allocation is in DTCM", DTCM_START <= addr < DTCM_END)

# Fill area 1 and the next allocation has to come from area 2. Sizing it by
# binary search rather than by a constant keeps this working as .bss moves.
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

check("area 1 is DTCM-sized", 120 * 1024 < lo < 200 * 1024)
ballast = bytearray(lo)
check("ballast is in DTCM", uctypes.addressof(ballast) < DTCM_END)

spill = bytearray(4096)
spill_addr = uctypes.addressof(spill)
print("spilled to", hex(spill_addr))
check("allocation spills into area 2", HEAP2_START <= spill_addr < HEAP2_END)
check("spilled block fits inside area 2", spill_addr + 4096 <= HEAP2_END)

# Area 2 is real memory, not a mapping of something else: write a pattern
# through the object and read it back, and confirm it did not alias over the
# ballast sitting in DTCM.
for i in range(0, 4096, 4):
    spill[i] = (i >> 2) & 0xFF
ok = True
for i in range(0, 4096, 4):
    if spill[i] != ((i >> 2) & 0xFF):
        ok = False
        break
check("area 2 holds what was written", ok)
check("ballast undisturbed", ballast[0] == 0 and ballast[lo - 1] == 0)

# The GC has to trace and reclaim in area 2 the same as in area 1.
del spill
gc.collect()
free_after = gc.mem_free()
spill2 = bytearray(4096)
check("area 2 memory is reused", uctypes.addressof(spill2) >= HEAP2_START)
del spill2, ballast
gc.collect()
check("releasing the ballast frees area 1", gc.mem_free() > free_after)

# Objects in area 2 must survive a collection while referenced, which is the
# part that breaks if an area is registered with the wrong bounds: the sweep
# would treat live blocks as unreachable.
gc.collect()
keep = []
for _ in range(8):
    b = bytearray(24 * 1024)
    b[0] = 0xA5
    b[-1] = 0x5A
    keep.append(b)
gc.collect()
in_area2 = sum(1 for b in keep if uctypes.addressof(b) >= HEAP2_START)
check("large allocations reach area 2", in_area2 > 0)
check("area 2 objects survive a collection", all(b[0] == 0xA5 and b[-1] == 0x5A for b in keep))
del keep
gc.collect()

print("%u passed, %u failed" % (passed, failed))
