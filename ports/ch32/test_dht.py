"""Verify the DHT11 bit-banged driver on PB5.

This doubles as an absolute check on the microsecond timebase. The DHT
protocol's timing is set by the sensor, not the board, so decoding a frame with
a valid checksum can only happen if time_pulse_us() is measuring in real
microseconds. When SysTick was misconfigured and ticks ran 4x slow, every 70 us
high measured as 17 us, fell under the 48 us one/zero threshold, and the frame
decoded as forty zero bits -- which is especially nasty because an all-zero
DHT11 frame passes its own checksum (0+0+0+0 == 0). So this test rejects an
all-zero frame explicitly rather than trusting the checksum alone.

It also checks that micropython-lib's dht.py is frozen into the firmware and
works, since that is what a user reaches for and a reflash leaves nothing on
the filesystem.

DHT11s are unreliable by nature: a fraction of reads time out no matter what
the host does. The pass criterion is therefore "most reads work", not "all".

Skips cleanly when no sensor is attached.
"""

import sys, os, time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import pyboard

PORT = os.environ.get("MPY_PORT", "COM9")

DEVICE_TEST = """
import time, machine
from machine import Pin

PIN = Pin.cpu.PB5
fail = []

# A DHT11 module pulls the data line up. Without that there is nothing to talk
# to, and the driver would just time out.
PIN.init(Pin.IN, Pin.PULL_DOWN)
time.sleep_ms(5)
present = PIN.value() == 1
print("PRESENT", present)

if present:
    # The low-level half first: machine.dht_readinto directly, so a failure here
    # points at the C driver and the timebase rather than at anything Python.
    buf = bytearray(5)

    good = 0
    zero_frames = 0
    last = None
    for i in range(8):
        time.sleep_ms(2000)          # DHT11 needs > 1 s between conversions
        try:
            machine.dht_readinto(PIN, buf)
        except OSError:
            continue
        if (buf[0] + buf[1] + buf[2] + buf[3]) & 0xFF != buf[4]:
            continue
        if buf[0] == 0 and buf[2] == 0:
            # Passes the checksum but means the bit timing collapsed.
            zero_frames += 1
            continue
        good += 1
        last = (buf[2], buf[0])

    if zero_frames:
        fail.append("%d all-zero frames -- microsecond timebase is wrong" % zero_frames)
    if good < 4:
        fail.append("only %d of 8 reads decoded" % good)
    if last is not None:
        temp, hum = last
        print("last reading: %d C, %d %% RH" % (temp, hum))
        # DHT11's own range; anything outside means the decode is garbage.
        if not (0 <= temp <= 50):
            fail.append("temperature %d out of the DHT11 range" % temp)
        if not (0 <= hum <= 100):
            fail.append("humidity %d out of range" % hum)

    # And the module a user actually reaches for. dht.py is frozen into the
    # firmware rather than installed, so this has to work on a board whose
    # filesystem was just erased by a reflash -- which is the state this test
    # runs in. Importing it off the filesystem instead would still pass, so
    # check where it came from as well.
    import os as _os

    if "dht.py" in _os.listdir("/"):
        fail.append("dht.py is on the filesystem, so this proves nothing")
    try:
        import dht
    except ImportError:
        fail.append("dht module is not frozen into the firmware")
        dht = None

    if dht is not None:
        d = dht.DHT11(PIN)
        ok = False
        for i in range(4):
            time.sleep_ms(2000)
            try:
                d.measure()
            except Exception:
                # OSError on timeout, a bare Exception on a bad checksum.
                continue
            ok = True
            print("dht module: %d C, %d %% RH" % (d.temperature(), d.humidity()))
            break
        if not ok:
            fail.append("frozen dht module never completed a reading")

print("FAILURES:", fail)
"""


def open_board():
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
        out = pyb.exec_(DEVICE_TEST, timeout=120).decode()
    finally:
        pyb.exit_raw_repl()
        pyb.close()
    sys.stdout.write(out)
    if "PRESENT True" not in out:
        print("DHT SKIP (no DHT11 on PB5)")
        return
    if "FAILURES: []" not in out:
        raise SystemExit("DHT FAIL")
    print("DHT PASS")


if __name__ == "__main__":
    main()
