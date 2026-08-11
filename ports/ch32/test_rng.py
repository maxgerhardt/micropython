"""os.urandom() and random, backed by the hardware TRNG.

These are statistical checks, so the thresholds are deliberately loose enough
that a working generator never trips them by chance, while still catching the
failure this driver actually had: raw RNG words carry only about 10 bits of
entropy each, so before whitening a 32-bit draw repeated far more often than
it should. See the measurements at the top of rng.c.

Run on the target: python scripts/install_file.py ports/ch32/test_rng.py /hwtest/test_rng.py
"""

import gc
import os
import random

passed = 0
failed = 0


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print("FAIL", name, detail)


# --- os.urandom: shape ---
check("urandom returns the length asked for", len(os.urandom(17)) == 17)
check("urandom(0) is empty", os.urandom(0) == b"")
check("urandom returns bytes", isinstance(os.urandom(4), bytes))
check("two calls differ", os.urandom(16) != os.urandom(16))

try:
    os.urandom(-1)
    check("urandom rejects negative", False)
except ValueError:
    check("urandom rejects negative", True)

# --- distribution. 2000 32-bit draws from a uniform source collide with
# probability ~0.0005, so even one collision is suspicious and three is a
# genuine signal. Unconditioned, this returned ~1400 distinct values.
gc.collect()
seen = set()
for _ in range(2000):
    seen.add(os.urandom(4))
check("2000 32-bit draws are distinct", len(seen) >= 1997, "got %d" % len(seen))
del seen
gc.collect()

# Bit balance. 8192 bytes is 65536 bits; a fair source lands within about
# 4 sigma (= 512) of half almost always.
ones = 0
for _ in range(4):
    ones += sum(bin(b).count("1") for b in os.urandom(2048))
check("bits are balanced", abs(ones - 32768) < 1024, "got %d of 65536" % ones)

# Every byte value should show up in a large enough sample.
gc.collect()
buckets = bytearray(256)
for b in os.urandom(4096):
    if buckets[b] < 255:
        buckets[b] += 1
missing = sum(1 for v in buckets if v == 0)
check("all 256 byte values occur", missing == 0, "%d missing" % missing)

# --- random: seeded from the TRNG, so it must not repeat across resets.
# Two draws differing only proves the PRNG runs; the seeding itself is checked
# by scripts/test_rng.py, which can compare across a reboot.
check("getrandbits differs", random.getrandbits(30) != random.getrandbits(30))
check("randint in range", all(1 <= random.randint(1, 6) <= 6 for _ in range(500)))
check("random() in [0,1)", all(0.0 <= random.random() < 1.0 for _ in range(300)))
check("randrange in range", all(0 <= random.randrange(10) < 10 for _ in range(300)))
check("choice picks a member", all(random.choice("abcdef") in "abcdef" for _ in range(200)))

# A d6 rolled 600 times should hit every face; a stuck generator would not.
faces = set(random.randint(1, 6) for _ in range(600))
check("d6 covers every face", len(faces) == 6, str(sorted(faces)))

# seed() must make the sequence reproducible -- that is the PRNG's contract,
# and it is what makes the TRNG seeding at import meaningful rather than
# decorative.
random.seed(12345)
a = [random.getrandbits(16) for _ in range(8)]
random.seed(12345)
b = [random.getrandbits(16) for _ in range(8)]
check("seed makes it reproducible", a == b)
random.seed(54321)
c = [random.getrandbits(16) for _ in range(8)]
check("different seed differs", a != c)

print("PASS", passed, "FAIL", failed)
print("RESULT", "OK" if failed == 0 else "FAILED")
