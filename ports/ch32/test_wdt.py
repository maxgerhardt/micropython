"""Verify machine.WDT on the CH32H417 by letting it actually reset the board.

The only honest test of a watchdog is that it resets the chip when it is not
fed and does not when it is, so this drives the board over raw serial rather
than through pyboard's raw REPL -- the board disappears mid-command by design,
which the raw REPL protocol treats as a failure.

Runs on the UART console (COM7), not USB CDC: a reset drops the CDC endpoint
and the host has to re-enumerate it, which is slow and would swamp the timing.

Every marker the host waits for is assembled on the board from chr() calls, so
that the REPL's echo of the command cannot contain the marker and satisfy the
match before the board has run anything. Getting that wrong makes the whole
test pass or fail on the echo instead of on the hardware.

The timing doubles as a measurement of the LSI, whose datasheet spread is
25-60 kHz. The elapsed time between the last feed and the boot banner gives
the real frequency, which is worth knowing before trusting a nominal timeout.
"""

import os
import re
import sys
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(ROOT, "..", "..", "..", "scripts"))
from serial_expect import ReplSession, reset_target  # noqa: E402

PORT = os.environ.get("MPY_PORT", "COM7")
BANNER = r"MicroPython on"

fail = []


def check(cond, what):
    if not cond:
        fail.append(what)
    return cond


def token(word):
    """A Python expression that builds `word` at run time."""
    return "+".join("chr(%d)" % ord(c) for c in word)


def fresh(s):
    """Back to a known prompt, whatever state the board is in."""
    s.interrupt()
    s.expect(r">>>", timeout=15)


def run(s, statement, marker, timeout=20, also=None):
    """Send one statement, wait for a marker the echo cannot contain."""
    s.send(statement)
    pattern = marker if also is None else "%s|%s" % (marker, also)
    return s.expect(pattern, timeout=timeout)


def time_to_reset(s, timeout):
    """Feed, announce, and time how long until the board comes back up.

    Returns seconds, which is the watchdog period plus a fixed overhead: the
    reset itself, the V3F stub waking the V5F, and the boot up to the banner.
    The caller cancels that overhead by differencing two timeouts.
    """
    s.drain(0.3)
    # Feed and announce in the same statement, so the watchdog's clock starts
    # where the measurement does. Announcing first and feeding earlier would
    # silently subtract however long the host took to get here -- which it did,
    # reporting 0.4 s for a 1 s watchdog.
    #
    # The read timeout has to come down for the duration: ReplSession polls the
    # port in 0.2 s blocks, so the timestamp taken after the marker arrives can
    # be a fifth of a second late. That alone moved the LSI this test derives
    # from 39 kHz to 49 kHz between two runs of identical code.
    was = s.ser.timeout
    s.ser.timeout = 0.005
    try:
        s.send("w.feed(); print(%s)" % token("ARMED"))
        s.expect("ARMED", timeout=10)
        t0 = time.monotonic()
        try:
            s.expect(BANNER, timeout=timeout)
        except AssertionError:
            return None
        return time.monotonic() - t0
    finally:
        s.ser.timeout = was


def main():
    reset_target()
    with ReplSession(PORT) as s:
        fresh(s)
        s.send("import machine, time")
        s.expect(r">>>", timeout=10)

        # --- argument checking, none of which may start a watchdog ---
        for expr, why in (
            ("machine.WDT(1)", "a second watchdog"),
            ("machine.WDT(0, timeout=0)", "zero timeout"),
            ("machine.WDT(0, timeout=-5)", "negative timeout"),
            ("machine.WDT(0, timeout=99999)", "26 s ceiling"),
        ):
            out = run(
                s,
                'exec("try:\\n %s\\n print(%s)\\nexcept ValueError:\\n print(%s)")'
                % (expr, token("BAD"), token("GOOD")),
                "GOOD",
                also="BAD",
            )
            check("GOOD" in out, "%s was accepted" % why)
            s.expect(r">>>", timeout=10)
        print("argument checking done")

        # --- feeding keeps it alive ---
        out = run(s, "w = machine.WDT(0, timeout=1000); print(%s, w)" % token("REPR"), r"REPR .*")
        print(out.strip())
        out = run(
            s,
            'exec("for i in range(30):\\n time.sleep_ms(100)\\n w.feed()\\nprint(%s)")'
            % token("FED3S"),
            "FED3S",
            also=BANNER,
        )
        if check("FED3S" in out, "the board reset while the WDT was being fed"):
            print("fed for 3 s without a reset")

        # --- not feeding resets it ---
        t1 = time_to_reset(s, timeout=20)
        if check(t1 is not None, "the WDT never reset the board"):
            print("reset %.3f s after the last feed (asked for 1.000 s)" % t1)
            check(0.5 < t1 < 2.0, "reset after %.3f s, nowhere near the 1 s asked for" % t1)

        fresh(s)
        out = run(
            s,
            "import machine; print(%s, machine.reset_cause(), machine.WDT_RESET)" % token("CAUSE"),
            r"CAUSE \d+ \d+",
        )
        # Anchored on the marker: the boot banner is full of version numbers.
        got, want = (int(x) for x in re.search(r"CAUSE (\d+) (\d+)", out).groups())
        print("reset_cause", got, "WDT_RESET", want)
        check(got == want, "reset_cause was %d, expected WDT_RESET (%d)" % (got, want))

        # --- a longer timeout really is longer ---
        fresh(s)
        s.send("import machine, time")
        s.expect(r">>>", timeout=10)
        run(s, "w = machine.WDT(0, timeout=3000); print(%s)" % token("ARMED3"), "ARMED3")
        t3 = time_to_reset(s, timeout=20)
        if check(t3 is not None, "the 3 s WDT never reset the board"):
            print("reset %.3f s after the last feed (asked for 3.000 s)" % t3)
            check(2.0 < t3 < 5.0, "reset after %.3f s, nowhere near the 3 s asked for" % t3)

        # Two timeouts give the LSI without needing to know the boot overhead,
        # because the overhead is the same for both and cancels in the
        # difference. The nominal configuration is 2500 counts of LSI/16 for
        # the 1 s watchdog and 3750 counts of LSI/32 for the 3 s one, so
        # 40000 and 120000 LSI cycles.
        if t1 is not None and t3 is not None:
            lsi_hz = (120000 - 40000) / (t3 - t1)
            overhead = t1 - 40000 / lsi_hz
            print("=> LSI %.1f kHz, boot overhead %.0f ms" % (lsi_hz / 1000.0, overhead * 1000))
            check(
                25000 < lsi_hz < 60000,
                "derived LSI %.0f Hz is outside the datasheet's 25-60 kHz" % lsi_hz,
            )
            # The overhead is genuinely small -- a few ms -- so this figure is
            # mostly measurement noise, and a few ms negative is fine. What it
            # catches is the timing method being broken: reading the port in
            # 0.2 s blocks used to put this at -218 ms, which would mean the
            # board booted before it reset.
            check(
                -0.05 <= overhead < 0.5,
                "implied boot overhead %.0f ms is not physical; the timing is wrong"
                % (overhead * 1000),
            )

        # --- and a soft reset does not disarm it ---
        fresh(s)
        out = run(
            s, "import machine; print(%s, machine.reset_cause())" % token("CAUSE2"), r"CAUSE2 \d+"
        )
        got = int(re.search(r"CAUSE2 (\d+)", out).group(1))
        check(got == want, "second reset reported cause %d, expected %d" % (got, want))
        fresh(s)

    print("FAILURES:", fail)
    if fail:
        raise SystemExit("WDT FAIL")
    print("WDT PASS")


if __name__ == "__main__":
    main()
