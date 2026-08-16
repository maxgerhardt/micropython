# machine.ADC.read_timed() -- a block of samples at a rate a timer sets.
#
# No wiring needed, and no signal generator either. PA4 is DAC1's output and
# also ADC_IN4, so the board can produce a voltage it knows and then measure
# it. That makes the two peripherals check each other: a wrong channel, a dead
# trigger, or a DMA that moved nothing at all show up as samples that do not
# follow what the DAC was told to produce. Range checks alone would not --
# a buffer left full of zeros is in range.
#
# What read_timed() is for is the spacing. A Python loop around read_u16()
# samples at whatever rate the interpreter managed, which is neither known nor
# even; here a timer triggers each conversion and DMA carries the result away,
# so the interval is exact and the CPU does nothing during the capture.
import time
from array import array

import machine

PIN = "PA4"  # DAC1's output pin, and ADC_IN4 on the way back in

passed = 0
failed = 0


def check(name, cond):
    global passed, failed
    if cond:
        passed += 1
        print("  PASS ", name)
    else:
        failed += 1
        print("  FAIL ", name)


def raises(name, exc, fn):
    try:
        fn()
    except exc:
        check(name, True)
        return
    except Exception as e:
        check("%s (got %s: %s)" % (name, type(e).__name__, e), False)
        return
    check("%s (nothing raised)" % name, False)


def mean(buf):
    total = 0
    for v in buf:
        total += v
    return total / len(buf)


print("ADC.read_timed")
adc = machine.ADC(machine.Pin(PIN))
dac = machine.DAC(1)
buf = array("H", bytearray(2 * 512))

# --- what it refuses ---
raises(
    "a bytearray is refused, not filled with low bytes",
    ValueError,
    lambda: adc.read_timed(bytearray(512), 1000),
)
raises("an empty buffer is refused", ValueError, lambda: adc.read_timed(array("H"), 1000))
raises("freq must be positive", ValueError, lambda: adc.read_timed(buf, 0))
# The converter needs sample + conversion time per trigger, so there is a
# ceiling; asking past it is refused with the ceiling in the message rather
# than silently producing a capture that never fills.
raises("an impossible rate is refused", ValueError, lambda: adc.read_timed(buf, 5000000))

check("busy() is False before anything starts", adc.busy() is False)

# --- the rate is real ---
# The whole claim of read_timed() is that the spacing is the timer's, so this
# is the check that matters: a capture of n samples at f Hz takes n/f seconds.
for freq in (1000, 8000, 50000):
    t0 = time.ticks_us()
    adc.read_timed(buf, freq)
    took = time.ticks_diff(time.ticks_us(), t0)
    want = len(buf) * 1000000 // freq
    off = 100.0 * (took - want) / want
    print(
        "    %6d Hz: %d samples took %d us, expected %d (%+.1f%%)"
        % (freq, len(buf), took, want, off)
    )
    check("%d Hz capture takes n/f seconds" % freq, abs(off) < 5.0)

check("busy() is False after a blocking capture", adc.busy() is False)

# 200 kHz is far beyond what the longest sample time allows, so this only
# works if the sample time was shortened to fit the rate that was asked for.
t0 = time.ticks_us()
adc.read_timed(buf, 200000)
fast_us = time.ticks_diff(time.ticks_us(), t0)
want = len(buf) * 1000000 // 200000
check(
    "200 kHz works, so the sample time was chosen to fit (%d us vs %d)" % (fast_us, want),
    abs(fast_us - want) < want // 5,
)

# --- the samples are real ---
raw = [v for v in buf]
check("every sample is inside the 12-bit range", all(0 <= v <= 4095 for v in raw))
check("they are raw counts, not read_u16()'s 0-65535 scaling", max(raw) <= 4095)

# Follow the DAC. Two levels far enough apart that noise cannot confuse them,
# and both well inside the rails so nothing clips.
dac.write_uv(500000)
time.sleep_ms(5)
adc.read_timed(buf, 10000)
low = mean(buf)
low_u16 = adc.read_u16()

dac.write_uv(2500000)
time.sleep_ms(5)
adc.read_timed(buf, 10000)
high = mean(buf)
high_u16 = adc.read_u16()

print("    DAC 0.5 V -> mean %.0f counts (read_u16 %d)" % (low, low_u16))
print("    DAC 2.5 V -> mean %.0f counts (read_u16 %d)" % (high, high_u16))
check("the capture follows the DAC upwards", high > low + 1500)
# read_u16() scales the same 12-bit reading to 0-65535, so the two must agree
# once that is undone. This is what shows read_timed() is reading the same
# channel, and reading it correctly.
check("and agrees with read_u16() on the same pin (low)", abs(low - low_u16 / 16) < 100)
check("and agrees with read_u16() on the same pin (high)", abs(high - high_u16 / 16) < 100)

# --- the converter is handed back in one piece ---
# read_timed() switches the trigger to TIM3 and has to switch it back, or the
# next single conversion waits for a timer that is no longer running.
v = adc.read_u16()
check("read_u16() still works after a capture (%d)" % v, 0 < v < 65536)

# --- non-blocking, via irq() ---
done = []
adc.irq(lambda a: done.append(a))

t0 = time.ticks_us()
adc.read_timed(buf, 2000)  # 512 samples at 2 kHz is 256 ms of capture
start_us = time.ticks_diff(time.ticks_us(), t0)
check("with a callback read_timed() returns at once (%d us)" % start_us, start_us < 5000)
check("busy() is True while it runs", adc.busy() is True)
raises("a second capture raises EBUSY", OSError, lambda: adc.read_timed(buf, 2000))

deadline = time.ticks_add(time.ticks_ms(), 1000)
while not done and time.ticks_diff(deadline, time.ticks_ms()) > 0:
    time.sleep_ms(5)
check("the callback runs", len(done) == 1)
check("the callback is passed the ADC object", done and done[0] is adc)
check("busy() is False once it has finished", adc.busy() is False)
check("the buffer really was filled", mean(buf) > 0)

adc.irq(None)
t0 = time.ticks_us()
adc.read_timed(buf, 50000)
check("irq(None) restores blocking", time.ticks_diff(time.ticks_us(), t0) > 5000)

# --- it does not steal a timer someone else holds ---
t3 = machine.Timer(3, freq=10, callback=lambda t: None)
raises(
    "a capture refuses to take TIM3 from machine.Timer",
    ValueError,
    lambda: adc.read_timed(buf, 1000),
)
t3.deinit()
adc.read_timed(buf, 1000)
check("and works again once the timer is released", True)

dac.write_uv(0)
print("%u passed, %u failed" % (passed, failed))
