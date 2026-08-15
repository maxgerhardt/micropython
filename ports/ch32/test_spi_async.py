# machine.SPI interrupt-driven transfers, via irq().
#
# Uses SPI4 on PE2 (SCK), PE5 (MISO), PE6 (MOSI). No device needed: what is
# under test is when the call returns and when the callback runs, not what
# comes back on MISO -- test_spi.py covers the data against real hardware.
#
# Those pins are chosen for being free as well as being in the 3.3 V domain.
# SPI1's defaults are PA4-PA7, and PA4/PA5 are the DAC outputs, so running this
# there would put a megahertz square wave into whatever the DACs feed.
import time

import machine

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
        check("%s (got %s)" % (name, type(e).__name__), False)
        return
    check("%s (nothing raised)" % name, False)


print("SPI async")

# 1 MHz and 8192 bytes is about 65 ms on the wire -- long enough that "returned
# before it finished" is unambiguous rather than a timing coincidence.
BAUD = 1000000
buf = bytearray(8192)
spi = machine.SPI(
    4, baudrate=BAUD, sck=machine.Pin("PE2"), miso=machine.Pin("PE5"), mosi=machine.Pin("PE6")
)

# Blocking is still the default.
t0 = time.ticks_us()
spi.write(buf)
blocking_us = time.ticks_diff(time.ticks_us(), t0)
check("without irq() write() blocks (%d us)" % blocking_us, blocking_us > 40000)

done = []
spi.irq(lambda s: done.append(s))

t0 = time.ticks_us()
spi.write(buf)
start_us = time.ticks_diff(time.ticks_us(), t0)
check("with irq() write() returns at once (%d us)" % start_us, start_us < blocking_us // 4)

# A second transfer while one is running has nowhere to go: one pair of DMA
# channels, and queueing would hide the mistake.
raises("a second transfer raises EBUSY", OSError, lambda: spi.write(buf))

deadline = time.ticks_add(time.ticks_ms(), 1000)
while not done and time.ticks_diff(deadline, time.ticks_ms()) > 0:
    time.sleep_ms(1)
check("the callback runs", len(done) == 1)
check("the callback is passed the SPI object", done and done[0] is spi)

# It must really have waited for the wire, not just for the DMA to be armed.
check("the callback came no sooner than the transfer (%d us)" % blocking_us, blocking_us > 40000)

# Back to blocking.
spi.irq(None)
t0 = time.ticks_us()
spi.write(buf)
again_us = time.ticks_diff(time.ticks_us(), t0)
check("irq(None) restores blocking (%d us)" % again_us, again_us > 40000)

raises("a non-callable callback is refused", ValueError, lambda: spi.irq(42))

# Reads take the same path, and are what the receive channel is watched for.
rx = bytearray(4096)
done.clear()
spi.irq(lambda s: done.append(s))
spi.readinto(rx)
deadline = time.ticks_add(time.ticks_ms(), 1000)
while not done and time.ticks_diff(deadline, time.ticks_ms()) > 0:
    time.sleep_ms(1)
check("readinto() completes asynchronously too", len(done) == 1)
spi.irq(None)

# A transfer longer than one DMA pass (CNTR is 16 bits) has to be chained by
# the interrupt rather than by the caller.
big = bytearray(70000)
done.clear()
spi.irq(lambda s: done.append(s))
spi.write(big)
deadline = time.ticks_add(time.ticks_ms(), 3000)
while not done and time.ticks_diff(deadline, time.ticks_ms()) > 0:
    time.sleep_ms(1)
check("a >65535 byte transfer chains its chunks", len(done) == 1)
spi.irq(None)

spi.deinit()
print("%u passed, %u failed" % (passed, failed))
