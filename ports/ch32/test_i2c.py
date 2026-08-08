"""Verify machine.I2C against an SSD1306 OLED on I2C1 (SCL=PB6, SDA=PB7).

PB6/PB7 are the only I2C-capable pair on this part that sits in the 3.3 V
VDDIO domain; every other option is on VIO18, which comes up at 1.8 V.

The display is a good target for this because it exercises both directions:
long writes for the framebuffer, and a status byte whose bit 6 tracks whether
the panel is on, which makes a read verifiable rather than merely
well-formed.

Skips cleanly when nothing is attached, so it is safe to run unattended.
"""

import sys, os, time

sys.path.insert(
    0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "scripts")
)
from serial_expect import DEFAULT_PORT  # noqa: F401  (documents the port convention)

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import pyboard

PORT = os.environ.get("MPY_PORT", "COM9")
SSD1306_ADDR = 0x3C

DEVICE_TEST = """
import time
from machine import I2C, SoftI2C, Pin

ADDR = 0x3C
fail = []

def check(name, got, want):
    if got != want:
        fail.append("%s: got %r want %r" % (name, got, want))

i2c = I2C(1, freq=100000)
present = ADDR in i2c.scan()
print("PRESENT", present)

if present:
    # --- clock accuracy -------------------------------------------------
    # The vendor I2C_Init() derives its divider from HCLK, but this
    # peripheral runs from the system clock, which is 4x higher here. That
    # bug ran the bus at 4.00x the requested rate, so measure rather than
    # trust the register.
    payload = b"\\x40" + b"\\x00" * 1024
    bits = 1025 * 9 + 2
    for req in (100000, 400000):
        bus = I2C(1, freq=req)
        bus.writeto(ADDR, payload)          # warm up
        t0 = time.ticks_us()
        for _ in range(4):
            bus.writeto(ADDR, payload)
        us = time.ticks_diff(time.ticks_us(), t0) / 4
        actual = bits / (us / 1e6)
        ratio = actual / req
        # Wide band on purpose. This times a ~90 ms window with USB and
        # SysTick interrupts live and includes any clock stretching the
        # display does, so a few percent of jitter is normal -- one run
        # measured 0.89x while six others sat at 1.00x. The regression being
        # guarded against is the 4.00x clock-source error, which this catches
        # with room to spare.
        if not (0.75 < ratio < 1.3):
            fail.append("SCL %d Hz: measured %d Hz (%.2fx)" % (req, actual, ratio))

    i2c = I2C(1, freq=400000)

    # --- reads reflect device state -------------------------------------
    # Bit 6 of the SSD1306 status byte is set while the panel is off, so
    # this proves the read returned the device's answer and not a stale
    # buffer or our own bus state.
    i2c.writeto(ADDR, b"\\x00\\xAF")
    time.sleep_ms(20)
    on = i2c.readfrom(ADDR, 1)[0]
    i2c.writeto(ADDR, b"\\x00\\xAE")
    time.sleep_ms(20)
    off = i2c.readfrom(ADDR, 1)[0]
    i2c.writeto(ADDR, b"\\x00\\xAF")
    check("status bit6 on", (on >> 6) & 1, 0)
    check("status bit6 off", (off >> 6) & 1, 1)

    # --- every receive length takes a different code path ----------------
    # 1, 2 and >2 bytes each need their own ACK/STOP ordering on this
    # peripheral; a naive shared loop acknowledges one byte too many.
    for n in (1, 2, 3, 5, 32):
        check("readfrom(%d) length" % n, len(i2c.readfrom(ADDR, n)), n)

    # --- writes of assorted lengths --------------------------------------
    i2c.writeto(ADDR, b"\\x00\\xAF")            # 2 bytes
    i2c.writeto(ADDR, b"\\x00\\x21\\x00\\x7F")  # 4 bytes
    i2c.writeto(ADDR, b"\\x40" + b"\\x00" * 1024)

    # --- a NACK must be reported, not hang -------------------------------
    try:
        i2c.writeto(0x7A, b"\\x00")
        fail.append("write to an empty address should have raised")
    except OSError:
        pass

    # --- SoftI2C sees the same device ------------------------------------
    # It bit-bangs the pins, so it reconfigures them away from the I2C
    # alternate function; the hardware bus has to be re-created afterwards.
    soft = SoftI2C(scl=Pin.cpu.PB6, sda=Pin.cpu.PB7, freq=100000)
    check("SoftI2C finds the device", ADDR in soft.scan(), True)
    i2c = I2C(1, freq=400000)
    check("hardware I2C works again", ADDR in i2c.scan(), True)

print("FAILURES:", fail)
"""


def open_board():
    # The CDC port vanishes across a reset or a reflash and takes a second or
    # two to come back, so opening it immediately after one is a race.
    last = None
    for _ in range(20):
        try:
            return pyboard.Pyboard(PORT, 115200)
        except Exception as exc:
            last = exc
            time.sleep(1.0)
    raise SystemExit("could not open %s: %s" % (PORT, last))


def main():
    pyb = open_board()
    pyb.enter_raw_repl()
    try:
        out = pyb.exec_(DEVICE_TEST).decode()
    finally:
        pyb.exit_raw_repl()
        pyb.close()
    sys.stdout.write(out)
    if "PRESENT True" not in out:
        print("I2C SKIP (no SSD1306 at 0x%02X on PB6/PB7)" % SSD1306_ADDR)
        return
    if "FAILURES: []" not in out:
        raise SystemExit("I2C FAIL")
    print("I2C PASS")


if __name__ == "__main__":
    main()
