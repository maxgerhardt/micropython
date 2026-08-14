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

print("%u passed, %u failed" % (passed, failed))
