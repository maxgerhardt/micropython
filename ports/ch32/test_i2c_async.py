# machine.I2C interrupt-driven transfers, via irq().
#
# Wants the same SSD1306 on I2C1 (SCL=PB6, SDA=PB7) that test_i2c.py uses, and
# for the same reason: bit 6 of its status byte tracks whether the panel is on,
# so an asynchronous write can be shown to have actually reached the device
# rather than merely to have been started. Skips cleanly with nothing attached.
import time

import machine

ADDR = 0x3C
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


print("I2C async")
i2c = machine.I2C(1, scl=machine.Pin("PB6"), sda=machine.Pin("PB7"), freq=400000)

if ADDR not in i2c.scan():
    print("  SKIP  no SSD1306 at 0x3c on PB6/PB7")
    raise SystemExit


def status():
    return i2c.readfrom(ADDR, 1)[0]


def wait(flag, ms=1000):
    deadline = time.ticks_add(time.ticks_ms(), ms)
    while not flag and time.ticks_diff(deadline, time.ticks_ms()) > 0:
        time.sleep_ms(1)
    return bool(flag)


# Blocking still works, and gives the reference for what the bits mean.
i2c.writeto(ADDR, b"\x00\xaf")
check("blocking write turns the panel on", (status() >> 6) & 1 == 0)
i2c.writeto(ADDR, b"\x00\xae")
check("blocking write turns the panel off", (status() >> 6) & 1 == 1)

# A framebuffer's worth, to have something long enough to time: 1025 bytes at
# 400 kHz is about 25 ms.
frame = b"\x40" + b"\x00" * 1024
t0 = time.ticks_us()
i2c.writeto(ADDR, frame)
blocking_us = time.ticks_diff(time.ticks_us(), t0)
check("a long blocking write takes real time (%d us)" % blocking_us, blocking_us > 10000)

done = []
i2c.irq(lambda b: done.append(b))

# --- writes ---
t0 = time.ticks_us()
i2c.writeto(ADDR, frame)
start_us = time.ticks_diff(time.ticks_us(), t0)
check("with irq() a long write returns at once (%d us)" % start_us, start_us < blocking_us // 4)
raises("a second transfer raises EBUSY", OSError, lambda: i2c.writeto(ADDR, frame))
check("the write callback runs", wait(done))
check("the callback is passed the I2C object", done and done[0] is i2c)

# The point of the whole exercise: the bytes really went out.
done.clear()
i2c.writeto(ADDR, b"\x00\xaf")
check("short async write completes", wait(done))
i2c.irq(None)
check("the async write reached the device (panel on)", (status() >> 6) & 1 == 0)

i2c.irq(lambda b: done.append(b))
done.clear()
i2c.writeto(ADDR, b"\x00\xae")
check("second short async write completes", wait(done))
i2c.irq(None)
check("and again (panel off)", (status() >> 6) & 1 == 1)

# --- reads ---
expected = status()
buf = bytearray(1)
done.clear()
i2c.irq(lambda b: done.append(b))
i2c.readfrom_into(ADDR, buf)
check("async read completes", wait(done))
check("async read returns what the blocking one did", buf[0] == expected)

# A multi-byte read exercises the DMA's automatic NACK on the last byte, which
# is what replaces the polled path's POS juggling.
big = bytearray(8)
done.clear()
i2c.readfrom_into(ADDR, big)
check("multi-byte async read completes", wait(done))

# scan() and a missing address stay synchronous: both are addressing, not data,
# and a caller still needs the answer and the exception respectively.
check("scan() still finds the device with a handler set", ADDR in i2c.scan())
raises("an absent address still raises", OSError, lambda: i2c.writeto(0x7A, b"\x00"))

i2c.irq(None)
t0 = time.ticks_us()
i2c.writeto(ADDR, frame)
again_us = time.ticks_diff(time.ticks_us(), t0)
check("irq(None) restores blocking (%d us)" % again_us, again_us > 10000)

i2c.writeto(ADDR, b"\x00\xaf")
print("%u passed, %u failed" % (passed, failed))
