"""Verify machine.ADC against an MCP4725 DAC driven over I2C.

The DAC sits on I2C1 (SCL=PB6, SDA=PB7) at 0x60 and its output is wired to
PA7, which is ADC_IN7. That makes the two peripherals check each other: I2C
commands a known voltage, the ADC has to agree. Neither can fake a pass on its
own -- a broken ADC reference, a wrong channel, or a dropped I2C write all show
up as divergence.

Skips cleanly when the DAC is not attached.
"""

import sys, os, time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import pyboard

PORT = os.environ.get("MPY_PORT", "COM9")

DEVICE_TEST = """
import time
from machine import I2C, Pin, ADC

DAC = 0x60
FULL_SCALE_MV = 3300      # VREFP is tied to the 3.3 V rail on this board
fail = []

def check(name, got, want):
    if got != want:
        fail.append("%s: got %r want %r" % (name, got, want))

i2c = I2C(1, freq=400000)
present = DAC in i2c.scan()
print("PRESENT", present)

if present:
    adc = ADC(Pin.cpu.PA7)

    def set_dac(code):
        # MCP4725 fast-mode write; deliberately not the EEPROM form, which
        # would wear the part out over a test suite.
        i2c.writeto(DAC, bytes([(code >> 8) & 0x0F, code & 0xFF]))

    def read_mv(n=8):
        return sum(adc.read_uv() for _ in range(n)) // n // 1000

    # --- agreement across the range ---
    worst = 0
    readings = []
    for code in range(0, 4096, 256):
        set_dac(code)
        time.sleep_ms(20)
        got = read_mv()
        readings.append(got)
        expect = code * FULL_SCALE_MV // 4095
        worst = max(worst, abs(got - expect))
    # 60 mV is ~1.8% of full scale: loose enough for DAC offset, ADC noise and
    # the 3.3 V rail not being exactly 3.300 V, tight enough that a wrong
    # channel, a stuck bus or a bad reference cannot slip through.
    if worst > 60:
        fail.append("worst DAC/ADC divergence %d mV" % worst)

    # --- monotonic ---
    # Catches a scrambled channel or a stuck conversion, which can still land
    # inside the tolerance band above by luck.
    for i in range(1, len(readings)):
        if readings[i] < readings[i - 1] - 20:
            fail.append("not monotonic at step %d: %d then %d"
                        % (i, readings[i - 1], readings[i]))
            break

    # --- endpoints ---
    set_dac(0)
    time.sleep_ms(50)
    lo = read_mv()
    set_dac(4095)
    time.sleep_ms(50)
    hi = read_mv()
    if lo > 60:
        fail.append("zero-scale reads %d mV" % lo)
    if hi < FULL_SCALE_MV - 120:
        fail.append("full-scale reads %d mV" % hi)

    # --- read_u16 spans the 16-bit range ---
    set_dac(0)
    time.sleep_ms(50)
    u16_lo = adc.read_u16()
    set_dac(4095)
    time.sleep_ms(50)
    u16_hi = adc.read_u16()
    if u16_lo > 2000:
        fail.append("read_u16 at zero scale = %d" % u16_lo)
    if u16_hi < 63000:
        fail.append("read_u16 at full scale = %d" % u16_hi)

    # --- a pin with no analog function must be rejected ---
    try:
        ADC(Pin.cpu.PB6)
        fail.append("PB6 has no ADC channel and should have raised")
    except ValueError:
        pass

    set_dac(0)

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
        out = pyb.exec_(DEVICE_TEST).decode()
    finally:
        pyb.exit_raw_repl()
        pyb.close()
    sys.stdout.write(out)
    if "PRESENT True" not in out:
        print("ADC SKIP (no MCP4725 at 0x60 with its output on PA7)")
        return
    if "FAILURES: []" not in out:
        raise SystemExit("ADC FAIL")
    print("ADC PASS")


if __name__ == "__main__":
    main()
