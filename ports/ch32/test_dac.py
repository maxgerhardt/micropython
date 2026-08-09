"""Verify machine.DAC against the on-chip ADC, with PA4 wired to PA6.

DAC1_OUT is PA4 and DAC2_OUT is PA5; both are fixed-function analog pins with
no mux to move them. PA6 is ADC_IN6, and PA4/PA5/PA6 are all in the 3.3 V
supply domain, so the loopback needs no level shifting.

    PA4 (DAC1_OUT) ---- PA6 (ADC_IN6)

Two independent converters checking each other, the same arrangement
test_adc.py uses with an external MCP4725 -- except here both are on the die,
so a systematic reference error would cancel and go unnoticed. What this
proves is linearity and monotonicity, plus that the two agree on full scale;
test_adc.py against an external part is what pins the absolute scale down.

Skips cleanly when the wire is not fitted.
"""

import sys, os, time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import pyboard

PORT = os.environ.get("MPY_PORT", "COM9")

DEVICE_TEST = """
import time
from machine import Pin, ADC, DAC

fail = []

def check(cond, what):
    if not cond:
        fail.append(what)

d = DAC(1)
print("repr:", d)

# The ADC has to exist before anything drives the pin analog; creating it is
# what puts PA6 into analog mode.
a = ADC(Pin.cpu.PA6)

def settle_read(code, n=8):
    d.write(code)
    time.sleep_ms(5)              # the amplifier settles in ~4 us; be generous
    v = sorted(a.read_uv() for _ in range(n))
    return v[n // 2]

# Is the wire actually there? Full scale and zero must differ by most of the
# rail; an unconnected PA6 floats and reads nothing like this.
lo = settle_read(0)
hi = settle_read(4095)
print("code 0 -> %d uV, code 4095 -> %d uV" % (lo, hi))
if hi - lo < 2000000:
    print("DAC LOOPBACK ABSENT")
else:
    print("FULL SCALE %d uV" % hi)
    check(lo < 60000, "code 0 read %d uV, expected near ground" % lo)
    check(hi > 3200000, "code 4095 read %d uV, expected near 3.3 V" % hi)

    # Linearity across the range. The DAC's own full scale is VREFP, and so is
    # the ADC's, so expected microvolts are code * 3300000 / 4095.
    print("%6s %10s %10s %8s" % ("code", "expect_uV", "read_uV", "err_mV"))
    worst = 0
    prev = -1
    for code in (0, 256, 512, 1024, 2048, 3072, 3584, 4095):
        expect = code * 3300000 // 4095
        got = settle_read(code)
        err = abs(got - expect)
        if err > worst:
            worst = err
        print("%6d %10d %10d %8.1f" % (code, expect, got, (got - expect) / 1000.0))
        check(got > prev, "output not monotonic at code %d" % code)
        prev = got
    print("worst error %.1f mV" % (worst / 1000.0))
    # 12-bit DAC into a 12-bit ADC, both referenced to the same rail: an LSB is
    # 0.8 mV and each converter has a few LSB of offset and gain error.
    check(worst < 40000, "worst error %d uV is too large" % worst)

    # write_uv() is this port's addition; it must agree with write().
    d.write_uv(1650000)
    time.sleep_ms(5)
    mid = sorted(a.read_uv() for _ in range(8))[4]
    print("write_uv(1650000) -> %d uV" % mid)
    check(abs(mid - 1650000) < 40000, "write_uv gave %d uV" % mid)

    # A small step must actually move the output: this is a real converter,
    # not 8 bits in a 12-bit costume.
    a1 = settle_read(2048)
    a2 = settle_read(2048 + 16)     # 16 LSB = ~12.9 mV
    print("16-LSB step: %d -> %d uV (%.1f mV)" % (a1, a2, (a2 - a1) / 1000.0))
    check(a2 - a1 > 8000, "a 16-LSB step moved the output by only %d uV" % (a2 - a1))

# The second converter exists and is independent, even though nothing is
# listening to PA5.
e = DAC(Pin.cpu.PA5)
print("second:", e)
check("DAC(2" in repr(e), "PA5 did not map to DAC2")
e.write(2048)
e.deinit()
try:
    e.write(0)
    fail.append("write() worked on a deinitialised DAC")
except ValueError:
    pass

try:
    DAC(Pin.cpu.PA7)
    fail.append("a pin with no DAC was accepted")
except ValueError:
    pass
try:
    DAC(1).write(4096)
    fail.append("value 4096 was accepted")
except ValueError:
    pass
try:
    DAC(3)
    fail.append("DAC(3) was accepted")
except ValueError:
    pass

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
    if "DAC LOOPBACK ABSENT" in out:
        print("DAC SKIP (no wire from PA4 to PA6)")
        return
    if "FAILURES: []" not in out:
        raise SystemExit("DAC FAIL")
    print("DAC PASS")


if __name__ == "__main__":
    main()
