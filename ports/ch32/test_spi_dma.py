"""Verify the DMA transfer path of machine.SPI on SPI1 (SCK=PA5, MISO=PA6, MOSI=PA7).

Needs no device on the bus. Nothing here looks at the protocol -- test_spi.py
does that against a real sensor -- and everything here is about the transport:
does a transfer reach the rate the prescaler implies, does it move every byte,
and does it still work at the lengths that switch paths.

The throughput check is the point of the file. A polling loop passed the
protocol tests perfectly while delivering a quarter of the line rate at 50 MHz
and losing bytes outright above 6.25 MHz, because a correct-looking small
transfer says nothing about either. Measuring the rate is what caught it.

Two optional wires make more of it run, and it says which it found:

  * MOSI (PA7) to MISO (PA6) -- a loopback, which turns the transport checks
    into data checks: what came back has to be what went out, in order.
  * PA4 to MISO (PA6) -- the link left over from the DAC test. Driving PA4
    proves the received bytes come from the pin and that the receive DMA fills
    the whole buffer, which is weaker than a loopback but better than nothing.

PA6 is an SPI input throughout, so PA4 is the only pin this ever drives -- it is
never possible for both ends of that wire to be outputs.
"""

import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import pyboard  # noqa: E402

PORT = os.environ.get("MPY_PORT", "COM7")

TEST = """
import gc, time
from machine import SPI, Pin

fail = []
checks = 0

def check(name, got, want):
    global checks
    checks += 1
    if got != want:
        fail.append("%s: got %r want %r" % (name, got, want))

spi = SPI(1, baudrate=1000000, polarity=0, phase=0)

# --- is a loopback fitted? --------------------------------------------------
# Send a pattern no floating or stuck line can produce and see if it returns.
probe = bytes((0x5A, 0xA5, 0x0F, 0xF0, 0x01, 0x80, 0x7E, 0x3C))
back = bytearray(len(probe))
spi.write_readinto(probe, back)
loopback = bytes(back) == probe
print("LOOPBACK", loopback)

# --- throughput, which is the whole reason this file exists -----------------
# Below 90% something is stalling SCK between bytes; polling managed 25% at
# 50 MHz. Allow a little slack for the fixed per-call cost.
N = 65536
out = bytes(N)
buf = bytearray(N)
for req in (3125000, 6250000, 12500000, 25000000, 50000000):
    spi = SPI(1, baudrate=req)
    hz = int(str(spi).split("baudrate=")[1].split(",")[0])
    for name in ("write", "readinto", "write_readinto"):
        t = time.ticks_us()
        if name == "write":
            spi.write(out)
        elif name == "readinto":
            spi.readinto(buf)
        else:
            spi.write_readinto(out, buf)
        us = time.ticks_diff(time.ticks_us(), t)
        pct = (N * 8 * 100) // (hz * us // 1000000)
        print("RATE %9d %-14s %4d%%" % (hz, name, pct))
        check("%s at %d Hz reaches the line rate" % (name, hz), pct >= 90, True)

# --- a buffer longer than the DMA counter -----------------------------------
# CNTR is 16 bits, so anything past 65535 has to be split into several passes.
# Off-by-one here would silently truncate or repeat a chunk.
spi = SPI(1, baudrate=25000000)
big = bytearray(70000)
t = time.ticks_us()
spi.readinto(big)
us = time.ticks_diff(time.ticks_us(), t)
# A chunking bug that stopped early would show up as a transfer far too quick.
expect = len(big) * 8 * 1000000 // 25000000
print("CHUNKED %d bytes in %d us (expected about %d)" % (len(big), us, expect))
check("a transfer past 65535 bytes takes as long as it should",
      expect < us < expect * 2, True)
# The sweep below allocates pairs of buffers as large as this one, and the heap
# is not big enough to hold those and these at once.
del big, out, buf
gc.collect()

# --- the lengths that switch between the DMA and polled paths ---------------
# Writes below 16 bytes are pushed out by hand, everything else goes through
# DMA, and reads always do. Walk across the boundary, and across the 65535-byte
# chunk boundary, at the slowest and fastest clocks: a path that only works at
# one end of the range is the failure mode this driver actually had.
#
# The two buffers are allocated once at the largest size and used through
# memoryview slices. Allocating a fresh pair per size instead exhausted the
# heap at 65537 bytes, and comparing with bytes(dst) == bytes(src) would double
# the cost again -- hence the tile-by-tile compare below, which never holds more
# than one tile.
MAX = 70000
TILE = bytes(((i * 37 + 13) & 0xFF) for i in range(251))

def fill(buf):
    off = 0
    while off < len(buf):
        end = min(off + len(TILE), len(buf))
        buf[off:end] = TILE[:end - off]
        off = end

def matches_tile(buf, n):
    # 251 is prime and does not divide 65535, so the pattern does not line up
    # with the DMA chunk boundary: a chunk transferred twice or in the wrong
    # order shifts the phase and shows up here.
    off = 0
    while off < n:
        end = min(off + len(TILE), n)
        if buf[off:end] != TILE[:end - off]:
            return False
        off = end
    return True

srcbuf = bytearray(MAX)
dstbuf = bytearray(MAX)
fill(srcbuf)

# Negative control. A comparison that cannot fail proves nothing, and the whole
# loopback half of this file rests on this one function.
fill(dstbuf)
check("the comparison accepts a correct buffer", matches_tile(dstbuf, MAX), True)
for spoil in (0, 250, 251, 65534, 65535, 65536, MAX - 1):
    was = dstbuf[spoil]
    dstbuf[spoil] = was ^ 0xFF
    check("the comparison rejects a buffer wrong at byte %d" % spoil,
          matches_tile(dstbuf, MAX), False)
    dstbuf[spoil] = was

for hz in (390625, 12500000, 50000000):
    spi = SPI(1, baudrate=hz)
    got = int(str(spi).split("baudrate=")[1].split(",")[0])
    for n in (1, 2, 3, 8, 15, 16, 17, 24, 64, 255, 256, 1024, 65534, 65535, 65536, 65537, 70000):
        if n > 4096 and got < 12500000:
            continue                    # minutes of wall time for no new path
        src = memoryview(srcbuf)[:n]
        dst = memoryview(dstbuf)[:n]
        spi.write(src)                  # write-only path
        spi.readinto(dst)               # receive with no source buffer
        if loopback:
            # readinto() sends zeros, so this also proves the buffer is not
            # simply left alone.
            check("readinto(%d) at %d Hz reads zeros back" % (n, got),
                  dstbuf[0], 0)
        # Poison the destination so a transfer that stops early cannot pass by
        # leaving bytes that happen to match.
        for i in range(0, n, 97):
            dstbuf[i] = 0xC3
        spi.write_readinto(src, dst)
        if loopback:
            check("loopback %d bytes at %d Hz" % (n, got), matches_tile(dstbuf, n), True)
        del src, dst

# read() returns a fresh buffer rather than filling one, and defaults to
# clocking out zeros unless told otherwise -- with a loopback that is visible.
spi = SPI(1, baudrate=25000000)
if loopback:
    check("read(64) clocks out zeros by default", spi.read(64), bytes(64))
    check("read(64, 0xA5) clocks out what it was told", spi.read(64, 0xA5), b"\\xA5" * 64)
    check("read(4) below the DMA threshold", spi.read(4, 0x3C), b"\\x3C" * 4)

# Zero length must be a no-op, not a DMA with a count of zero.
spi.write(b"")
spi.readinto(bytearray(0))
check("read(0) returns empty", spi.read(0), b"")

if not loopback:
    # No loopback. Fall back to the DAC test's PA4-PA6 wire if it is there:
    # drive PA4 and every received byte should follow it.
    spi = SPI(1, baudrate=12500000)
    drive = Pin('PA4', Pin.OUT)
    seen = []
    for level in (0, 1):
        drive.value(level)
        time.sleep_ms(2)
        buf = bytearray(b'\\x5A' * 4096)
        spi.readinto(buf)
        seen.append(set(buf))
    drive.init(Pin.IN)
    wired = seen[0] == {0x00} and seen[1] == {0xFF}
    print("PA4_WIRED", wired)
    if wired:
        check("every received byte follows MISO", True, True)
    else:
        print("NOTE no loopback and no PA4-PA6 wire: received data unverified")

print("CHECKS", checks)
print("FAILURES:", fail)
"""


def main():
    last = None
    for _ in range(10):
        try:
            pyb = pyboard.Pyboard(PORT, 115200)
            break
        except Exception as exc:
            last = exc
            time.sleep(1.0)
    else:
        raise SystemExit("could not open %s: %s" % (PORT, last))

    pyb.enter_raw_repl()
    try:
        out = pyb.exec_(TEST).decode()
    finally:
        pyb.exit_raw_repl()
        pyb.close()
    sys.stdout.write(out)

    if "FAILURES: []" not in out:
        raise SystemExit("SPI DMA FAIL")
    if "LOOPBACK True" not in out and "PA4_WIRED True" not in out:
        print("SPI DMA PASS (transport only: no loopback on PA7-PA6)")
    else:
        print("SPI DMA PASS")


if __name__ == "__main__":
    main()
