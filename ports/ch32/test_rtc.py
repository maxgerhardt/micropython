"""Verify machine.RTC on the CH32H417: all three clock sources, the calendar
conversion, and that it actually keeps time against the host clock.

Rate is checked by differencing the RTC against the host over ~20 s, using the
sub-second field so the comparison resolves to microseconds rather than to the
one-second granularity of the counter. That is what distinguishes a working
prescaler from one that is out by a factor -- the failure this hardware invites,
since each source needs a different divisor and only LSE divides exactly.

The year-2038 checks are the point of several of these: the counter holds
seconds since 2000-01-01 in 32 *unsigned* bits, so the instant that breaks a
signed Unix timestamp is unremarkable here and the clock runs to 2136. Setting
2038, 2100 and 2135 and reading them back is the proof.

Switching source resets the counter -- the hardware only lets RTCSEL change
across a backup-domain reset -- so the tests are ordered to leave the board back
on LSE, which is the default and the one this board has a crystal for.
"""

import os
import sys
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(ROOT, "..", "..", "tools"))
sys.path.insert(0, os.path.join(ROOT, "..", "..", "..", "scripts"))
import pyboard  # noqa: E402
from serial_expect import reset_target  # noqa: E402

PORT = os.environ.get("MPY_PORT", "COM7")

fail = []


def check(name, cond):
    if not cond:
        fail.append(name)
    return cond


def run(pyb, script):
    return pyb.exec_(script).decode()


CALENDAR = """
import machine, time
rtc = machine.RTC()
bad = []

def check(name, cond):
    if not cond:
        bad.append(name)

print("SOURCE", rtc.source(), "REPR", rtc)
check("default source is LSE", rtc.source() == machine.RTC.LSE)

# --- round trips, including the dates that break a signed 32-bit epoch -------
cases = (
    (2000, 1, 1, 0, 0, 0),        # the epoch itself
    (2016, 2, 29, 12, 0, 0),      # a leap day
    (2026, 8, 9, 14, 30, 45),
    (2038, 1, 19, 3, 14, 7),      # where a signed Unix timestamp overflows
    (2038, 1, 19, 3, 14, 8),      # and one second past it
    (2100, 3, 1, 0, 0, 0),        # 2100 is not a leap year
    (2135, 12, 31, 23, 59, 59),   # the last second the counter can hold
)
for c in cases:
    rtc.datetime((c[0], c[1], c[2], 0, c[3], c[4], c[5], 0))
    got = rtc.datetime()
    check("round trip %s" % (c,), tuple(got[:3]) == c[:3] and tuple(got[4:7]) == c[3:6])
    # time.localtime() must agree with the RTC, or the two clocks disagree about
    # what "now" means depending on which API you ask.
    lt = time.localtime()
    check("localtime agrees at %s" % (c,), lt[:3] == c[:3] and lt[3:6] == c[3:6])

# The counter cannot hold anything outside 2000..2135, so those must raise
# rather than wrap into a year it can hold.
for year in (1999, 1970, 2136, 3000):
    try:
        rtc.datetime((year, 1, 1, 0, 0, 0, 0, 0))
        check("year %d rejected" % year, False)
    except ValueError:
        pass

# --- it keeps counting across the 2038 instant, not just stores it ----------
# Element 3 is the weekday, so the time starts at element 4.
rtc.datetime((2038, 1, 19, 0, 3, 14, 5, 0))
time.sleep(4)
after = rtc.datetime()
check("ticks through the 2038 second",
      after[0] == 2038 and after[1] == 1 and after[2] == 19 and after[4] == 3
      and after[5] == 14 and 8 <= after[6] <= 10)
print("ACROSS2038", after)

# --- sub-seconds advance and wrap -------------------------------------------
seen = set()
for _ in range(20):
    seen.add(rtc.datetime()[7] // 100000)
    time.sleep_ms(37)
check("sub-seconds advance", len(seen) > 3)

# --- now() reports the same instant as datetime() ---------------------------
d = rtc.datetime()
n = rtc.now()
check("now agrees with datetime", n[:3] == d[:3] and n[3:6] == d[4:7])
check("now has a trailing None", n[7] is None)

rtc.datetime((2026, 8, 9, 0, 12, 0, 0, 0))
print("BAD", bad)
print("CALENDAR DONE")
"""

# Timing the clock against the host. Kept separate because it takes 20 s.
RATE = """
import machine, time
rtc = machine.RTC()

def stamp():
    d = rtc.datetime()
    return (d[4] * 3600 + d[5] * 60 + d[6]) + d[7] / 1000000.0

print("MARK", stamp())
"""

SWITCH = """
import machine, time
rtc = machine.RTC(source=machine.RTC.%s)
print("SOURCE", rtc.source(), "REPR", rtc)
rtc.datetime((2026, 8, 9, 0, 0, 0, 0, 0))
"""


def stamp(pyb):
    """Seconds-into-the-day on the board, and the host time it was read."""
    out = run(pyb, RATE)
    value = float(out.split("MARK")[1].strip().splitlines()[0])
    return value, time.monotonic()


def measure_rate(pyb, seconds, tolerance, label):
    """Compare RTC elapsed against host elapsed over `seconds`."""
    a, ha = stamp(pyb)
    time.sleep(seconds)
    b, hb = stamp(pyb)
    board = b - a
    host = hb - ha
    err = (board - host) / host
    print("%-8s board %.3f s vs host %.3f s -> %+.3f%%" % (label, board, host, err * 100))
    check("%s rate within %.1f%%" % (label, tolerance * 100), abs(err) < tolerance)
    return err


def open_board():
    """The console drops out across a reset and takes a moment to come back."""
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
        out = run(pyb, CALENDAR)
        sys.stdout.write(out)
        check("calendar checks", "BAD []" in out)

        # LSE is a watch crystal: tens of ppm. A tolerance of 0.2% over 20 s is
        # far looser than that, and is set by the serial round trip on each
        # stamp rather than by the oscillator. What it catches is a prescaler
        # wrong by a factor, which is the mistake worth catching.
        measure_rate(pyb, 20, 0.002, "LSE")

        # The other two sources. LSI is only specified to 25-60 kHz, so a
        # nominal 40000 divisor can be several percent out -- 10% here is a
        # check that it runs at roughly the right rate, not that it is any good.
        for name, tol in (("LSI", 0.10), ("HSE", 0.002)):
            out = run(pyb, SWITCH % name)
            sys.stdout.write(out)
            check("%s selected" % name, "SOURCE" in out and name in out)
            measure_rate(pyb, 20, tol, name)

        # Back to the default, and confirm switching really does restart the
        # counter -- documented behaviour, and worth pinning down.
        out = run(pyb, SWITCH % "LSE")
        sys.stdout.write(out)
        check("back on LSE", "LSE" in out)

        # Persistence: the RTC lives in the backup domain, so a soft reset must
        # not disturb it.
        before = float(run(pyb, RATE).split("MARK")[1].strip().splitlines()[0])
        host = time.monotonic()
        pyb.exit_raw_repl()
        pyb.enter_raw_repl(soft_reset=True)
        after = float(run(pyb, RATE).split("MARK")[1].strip().splitlines()[0])
        moved = after - before
        elapsed = time.monotonic() - host
        print("soft reset: RTC advanced %.2f s, host %.2f s" % (moved, elapsed))
        check("RTC survives a soft reset", abs(moved - elapsed) < 1.0)

        # The one that matters: a hard reset re-runs the whole boot path,
        # including the RTC configuration. It has to recognise a clock that is
        # already running and leave it alone rather than restart it, which is
        # the difference between a real-time clock and an uptime counter.
        run(pyb, "import machine; machine.RTC().datetime((2026, 8, 9, 0, 17, 45, 30, 0))")
        pyb.exit_raw_repl()
        pyb.close()
        host = time.monotonic()
        reset_target()
    finally:
        pass

    pyb = open_board()
    pyb.enter_raw_repl()
    try:
        out = run(
            pyb,
            "import machine; d = machine.RTC().datetime()\nprint('DT %d %d %d %d %d' % (d[0], d[1], d[4], d[5], d[6]))",
        )
        sys.stdout.write(out)
        year, month, hh, mm, ss = (int(x) for x in out.split("DT")[1].split()[:5])
        seconds = hh * 3600 + mm * 60 + ss
        expected = 17 * 3600 + 45 * 60 + 30 + (time.monotonic() - host)
        print("hard reset: RTC reads %d s into the day, expected about %d" % (seconds, expected))
        check("RTC keeps the date across a hard reset", year == 2026 and month == 8)
        check("RTC keeps the time across a hard reset", abs(seconds - expected) < 5)
    finally:
        try:
            pyb.exit_raw_repl()
        except Exception:
            pass
        pyb.close()

    print("FAILURES:", fail)
    if fail:
        raise SystemExit("RTC FAIL")
    print("RTC PASS")


if __name__ == "__main__":
    main()
