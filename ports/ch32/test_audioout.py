# machine.AudioOut -- streaming stereo audio out of the two 12-bit DACs.
#
# No wiring needed. The DMA's own consumption rate is the instrument: if the
# timer, the DAC trigger and the DMA are all working, frames leave the ring at
# exactly the sample rate, and if any one of them is misconfigured the rate is
# wrong by an obvious factor. Getting the timer clock wrong (core clock rather
# than HCLK) showed up here as every rate being 4x slow.
#
# To actually hear it: PA4 is left and PA5 is right, both raw DAC pins. Feed a
# high-impedance input through an RC low-pass on each -- 1k and 10nF is about
# right for 44.1 kHz -- and do not drive a speaker directly.
#
# Run on the target: python scripts/install_file.py ports/ch32/test_audioout.py
import math
import struct
import time

import asyncio
import machine

FRAMES = 4096

passed = 0
failed = 0


def check(name, cond):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print("FAIL", name)


def raises(name, exc, fn):
    try:
        fn()
    except exc:
        check(name, True)
        return
    except Exception as e:
        check(name + " (raised %s)" % type(e).__name__, False)
        return
    check(name + " (raised nothing)", False)


check("machine.AudioOut exists", hasattr(machine, "AudioOut"))
raises("rate has a floor", ValueError, lambda: machine.AudioOut(rate=10))
raises("rate has a ceiling", ValueError, lambda: machine.AudioOut(rate=400000))
raises("ibuf has a floor", ValueError, lambda: machine.AudioOut(ibuf=16))


def tone(rate, n, hz=1000):
    """n stereo frames of a sine, the channels in antiphase."""
    buf = bytearray()
    for i in range(n):
        s = int(20000 * math.sin(2 * math.pi * hz * i / rate))
        buf += struct.pack("<hh", s, -s)
    return buf


for rate in (8000, 22050, 44100):
    a = machine.AudioOut(rate=rate, ibuf=FRAMES)
    n = 1024
    buf = tone(rate, n)
    a.write(buf)

    # Frames the DMA has eaten = frames written - frames still in the ring.
    # Counting completed writes alone quantises the answer to the write size
    # and reads low by up to one block, which looked like a 23% rate error at
    # 8 kHz and a 2% one at 44.1 kHz -- a tell-tale that it was the yardstick.
    def in_ring():
        return FRAMES - 1 - a.free()

    written = 0
    before = in_ring()
    t0 = time.ticks_us()
    while time.ticks_diff(time.ticks_us(), t0) < 500000:
        if a.free() >= n:
            a.write(buf)
            written += n
    dt = time.ticks_diff(time.ticks_us(), t0)
    measured = (written + before - in_ring()) * 1000000 // dt
    err = abs(measured - rate) * 100.0 / rate
    print("  %6u Hz asked -> %6u Hz measured (%.2f%% off)" % (rate, measured, err))
    # 0.5% covers the integer reload: 100 MHz / 44100 is 2267.6, so the
    # closest achievable rate is 44091.7 Hz.
    check("%u Hz is delivered within 0.5%%" % rate, err < 0.5)
    check("free() stays inside the ring", 0 <= a.free() < FRAMES)
    a.deinit()

# --- shapes and lifecycle.
a = machine.AudioOut(rate=44100, ibuf=FRAMES)
check("a fresh ring is nearly all free", a.free() >= FRAMES - 8)
raises("odd buffer rejected", ValueError, lambda: a.write(bytearray(6)))
check("empty write is accepted", a.write(bytearray()) == 0)
check("write returns the byte count", a.write(tone(44100, 64)) == 64 * 4)
check("repr mentions the rate", "44100" in repr(a))

a.deinit()
check("repr says so once stopped", "stopped" in repr(a))
check("deinit is idempotent", a.deinit() is None)
raises("writing after deinit fails", OSError, lambda: a.write(tone(44100, 16)))

# The timer is shared with machine.Timer, and the claim has to be given back.
a = machine.AudioOut(rate=44100)
raises(
    "Timer(6) is refused while audio holds it",
    ValueError,
    lambda: machine.Timer(6, freq=10, callback=lambda t: None),
)
a.deinit()
t = machine.Timer(6, freq=1000, callback=lambda t: None)
check("Timer(6) works again after deinit", t is not None)
t.deinit()

# Underruns. The DMA never stops, so falling behind is silent in both senses:
# nothing raises, and since the free part of the ring is blanked, nothing is
# heard either. The counter is the only way to find out it happened.
#
# 4096 frames is 93 ms at 44.1 kHz, so a 300 ms pause guarantees the DMA runs
# past everything queued, while a second of keeping up must not register.
a = machine.AudioOut(rate=44100, ibuf=4096)
block = tone(44100, 1024)
check("a fresh AudioOut reports no underruns", a.underruns() == 0)

t0 = time.ticks_ms()
while time.ticks_diff(time.ticks_ms(), t0) < 1000:
    a.write(block)
check("keeping up reports no underruns", a.underruns() == 0)

time.sleep_ms(300)
a.write(block)
check("a stall longer than the ring is counted", a.underruns() == 1)

time.sleep_ms(300)
a.write(block)
check("a second stall counts once more", a.underruns() == 2)

t0 = time.ticks_ms()
while time.ticks_diff(time.ticks_ms(), t0) < 500:
    a.write(block)
check("recovering stops the count moving", a.underruns() == 2)

raises("irq() rejects a non-callable", ValueError, lambda: a.irq(42))
a.deinit()
raises("irq() after deinit raises", OSError, lambda: a.irq(None))

# --- irq(): write() stops blocking -------------------------------------------
#
# The callback rides the DMA's half-transfer and transfer-complete interrupts,
# so it means "another half-ring has been played, there is room now" rather
# than "your buffer has finished". Nothing ever finishes: the DMA circles.
RATE = 44100
a = machine.AudioOut(rate=RATE, ibuf=FRAMES)

# Long enough that it cannot fit in the ring, so a non-blocking write has to
# come back short and a blocking one has to take real time.
big = bytes(FRAMES * 4 * 3)

t0 = time.ticks_us()
a.write(big)
blocking_us = time.ticks_diff(time.ticks_us(), t0)
check("without a callback a long write blocks (%d us)" % blocking_us, blocking_us > 50000)

seen = []
a.irq(lambda obj: seen.append(obj))

t0 = time.ticks_us()
took = a.write(big)
start_us = time.ticks_diff(time.ticks_us(), t0)
check("with a callback it returns at once (%d us)" % start_us, start_us < blocking_us // 10)
check("and reports how much it accepted (%d of %d)" % (took, len(big)), 0 < took < len(big))
check("what it accepted is whole frames", took % 4 == 0)

deadline = time.ticks_add(time.ticks_ms(), 500)
while not seen and time.ticks_diff(deadline, time.ticks_ms()) > 0:
    time.sleep_ms(1)
check("the callback runs", len(seen) > 0)
check("the callback is passed the AudioOut object", seen and seen[0] is a)

# Cadence. Two callbacks per trip round the ring, and a trip is FRAMES/RATE --
# 93 ms at these settings, so about 21 a second. Checked as a range because
# the scheduler coalesces nothing but can be late.
seen.clear()
t0 = time.ticks_ms()
while time.ticks_diff(time.ticks_ms(), t0) < 1000:
    a.write(big)
    time.sleep_ms(5)
per_s = len(seen)
want = 2.0 * RATE / FRAMES
print("  callbacks: %d in 1 s, expected about %.0f" % (per_s, want))
check("callback rate is two per ring period", want * 0.7 < per_s < want * 1.3)

a.irq(None)
t0 = time.ticks_us()
a.write(big)
again_us = time.ticks_diff(time.ticks_us(), t0)
check("irq(None) restores blocking (%d us)" % again_us, again_us > 50000)
a.deinit()

# --- the point of it: feeding from asyncio without blocking ------------------
a = machine.AudioOut(rate=RATE, ibuf=FRAMES)
flag = asyncio.ThreadSafeFlag()
a.irq(lambda obj: flag.set())


async def feed(seconds):
    """Keep the ring full for a while, awaiting room instead of blocking."""
    mv = memoryview(block)
    off = 0
    total = 0
    ticks = 0
    t0 = time.ticks_ms()

    async def counter():
        nonlocal ticks
        while True:
            ticks += 1
            await asyncio.sleep_ms(1)

    beat = asyncio.create_task(counter())
    while time.ticks_diff(time.ticks_ms(), t0) < seconds * 1000:
        n = a.write(mv[off:])
        total += n
        off += n
        if off == len(block):
            off = 0
        else:
            await flag.wait()
    beat.cancel()
    return total, ticks, time.ticks_diff(time.ticks_ms(), t0)


sent, ticks, elapsed = asyncio.run(feed(2))
# Bytes have to match the rate: the DMA consumes exactly RATE frames a second
# whatever the producer does, so a feeder that kept up sent about that many.
# Expect to come in slightly under rather than over. The ring starts full of
# the silence put there at construction, and the DMA plays that without anyone
# having written it, so the first ring-full -- 93 ms of the run -- is consumed
# and never counted here.
expect = RATE * 4 * elapsed // 1000
print("  fed %d bytes in %d ms, expected about %d" % (sent, elapsed, expect))
check("an asyncio feeder sends audio at the sample rate", abs(sent - expect) < expect // 10)
check("and never underran", a.underruns() == 0)
# The whole reason for doing it this way: other tasks still got to run.
print("  a 1 ms task ran %d times meanwhile" % ticks)
check("the event loop stayed responsive", ticks > 500)
a.irq(None)
a.deinit()

print("%u passed, %u failed" % (passed, failed))
