# machine.bitstream() and the frozen neopixel driver.
#
# No LEDs are needed. Correctness here is timing: the port drives the pin from
# SysTick, which counts HCLK and *not* the V5F's core clock, and which reloads
# every millisecond rather than free-running. Getting either wrong still
# produces a waveform, just one no WS2812 will decode -- four times too fast
# for the first mistake, randomly short bits for the second. Both show up as
# the total transfer time being wrong, which is what this measures.
#
# It also checks the clock the measurement is made with. bitstream() masks
# interrupts, so SysTick's handler cannot run and the millisecond counter has
# to be corrected afterwards from the reloads the loop counted; without that a
# 13 ms transfer reads back as 0.9 ms and every measurement below passes for
# the wrong reason.
#
# Run on the target: python scripts/install_file.py ports/ch32/test_bitstream.py
import time

import machine
import neopixel

PIN = "PB5"

passed = 0
failed = 0


def check(name, cond):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print("FAIL", name)


def elapsed_us(fn):
    t0 = time.ticks_us()
    fn()
    return time.ticks_diff(time.ticks_us(), t0)


pin = machine.Pin(PIN, machine.Pin.OUT)

check("machine.bitstream exists", hasattr(machine, "bitstream"))

# --- the timing itself.
#
# The requested period is high + low, so n bytes take n * 8 * (high + low). The
# loop costs a few percent on top and is allowed to; what it must not do is
# come out short, or fast by a factor that would betray the wrong clock.
for high_ns, low_ns, count in ((2000, 2000, 32), (800, 450, 64), (400, 850, 128)):
    buf = bytes(count)
    want = count * 8 * (high_ns + low_ns) // 1000
    got = elapsed_us(lambda: machine.bitstream(pin, 0, (high_ns, low_ns, high_ns, low_ns), buf))
    print("  %d bytes at %d/%d ns: %d us, nominal %d" % (count, high_ns, low_ns, got, want))
    check("%d/%d ns period" % (high_ns, low_ns), want <= got <= want * 6 // 5)

# A one and a zero differ only in how long the pin is high, so a byte of each
# has to take the same time. If they do not, the two timings are being applied
# to the wrong phases.
timing = (400, 850, 800, 450)
zeros = elapsed_us(lambda: machine.bitstream(pin, 0, timing, bytes(200)))
ones = elapsed_us(lambda: machine.bitstream(pin, 0, timing, b"\xff" * 200))
print("  200 bytes: zeros %d us, ones %d us" % (zeros, ones))
check("zeros and ones take the same period", abs(zeros - ones) < 100)

# --- the millisecond clock survives a long transfer.
#
# 400 LEDs is 13 ms of masked interrupts, thirteen SysTick reloads. Compare the
# clock against itself across the write: ticks_ms and ticks_us come from the
# same counter but only ticks_us folds in the pending reload, so if the
# recovery is missing they disagree by milliseconds.
np = neopixel.NeoPixel(pin, 400)
ms0 = time.ticks_ms()
us = elapsed_us(np.write)
ms = time.ticks_diff(time.ticks_ms(), ms0)
print("  400 LEDs: %d us by ticks_us, %d ms by ticks_ms" % (us, ms))
check("long write is timed correctly", 12000 <= us <= 15000)
check("ticks_ms agrees with ticks_us", abs(ms - us // 1000) <= 1)

# --- the driver on top.
np = neopixel.NeoPixel(pin, 8)
check("NeoPixel length", len(np) == 8)
np[0] = (255, 0, 0)
np[7] = (0, 128, 64)
check("item assignment round-trips", np[0] == (255, 0, 0) and np[7] == (0, 128, 64))
# GRB on the wire, so the buffer is not in the order the tuple was given.
check("buffer is GRB", bytes(np.buf[0:3]) == b"\x00\xff\x00")
np.fill((1, 2, 3))
check("fill", all(np[i] == (1, 2, 3) for i in range(8)))
np.write()
check("write does not raise", True)

np4 = neopixel.NeoPixel(pin, 4, bpp=4)
np4[0] = (1, 2, 3, 4)
check("bpp=4 round-trips", np4[0] == (1, 2, 3, 4))
np4.write()

# Leave the pin as an input: it is shared with the DHT11 on this board.
pin.init(machine.Pin.IN)

print("%u passed, %u failed" % (passed, failed))
