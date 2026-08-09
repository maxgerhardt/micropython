"""Verify machine.PWM on the CH32H417 against the waveform it actually makes.

Nothing is wired for this. The pad's input buffer stays live while the pin is
in alternate-function mode, so machine.time_pulse_us() reads back the timer's
own output on the same pin -- which measures frequency and duty rather than
trusting the registers the driver just wrote. That distinction matters: an
early version of this driver had every register correct and was still measured
here, because the compare registers are preloaded and a duty set after the
timer starts does not reach the pin until the next update event.

Frequency and duty are only checked closely at or below 1 kHz.
time_pulse_us() loses several microseconds at the start of every pulse it
times -- it has to see the edge before it can start counting -- which is
invisible at 50 Hz and swamps the reading at 20 kHz, where a 50 us period
measures as 38 us. Above 1 kHz the test therefore claims only that the pin is
still toggling at roughly the right rate, and leans on the exact values
reported back from the registers. Fudging a tolerance wide enough to cover the
instrument's own error would not be measuring anything.

Pins were chosen after mapping the board. Avoided: PA4-PA7 (SPI1 and the
radio's CS), PB5 (DHT11), PB6/PB7 (I2C), PA9/PA10 (REPL UART), PA11/PA12
(USB), and PB8/PB9 -- which are SWCLK and SWDIO, held by the debug probe, so
a PWM on them is configured perfectly and never moves the pin. PB0 and PB1 are
tied together on this board, so only PB0 is driven; the second channel of the
shared-timer test is on PC6.
"""

import sys, os, time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import pyboard

PORT = os.environ.get("MPY_PORT", "COM9")

DEVICE_TEST = """
import machine, time
from machine import Pin, PWM

fail = []

def check(cond, what):
    if not cond:
        fail.append(what)

def measure(pin, cycles=7):
    '''Median high and low time in us, or None if the pin never moves.'''
    hi = []
    lo = []
    for _ in range(cycles):
        h = machine.time_pulse_us(pin, 1, 200000)
        l = machine.time_pulse_us(pin, 0, 200000)
        if h > 0 and l > 0:
            hi.append(h)
            lo.append(l)
    if len(hi) < 3:
        return None
    hi.sort()
    lo.sort()
    return hi[len(hi) // 2], lo[len(lo) // 2]

def duty_pct(m):
    return 100.0 * m[0] / (m[0] + m[1])

def freq_hz(m):
    return 1000000.0 / (m[0] + m[1])

# --- a plain channel: does the waveform match what was asked for? ---
PIN = Pin.cpu.PB0
p = PWM(PIN, freq=1000, duty_u16=16384)
print("repr:", p)
m = measure(PIN)
print("as constructed:", m)
check(m is not None, "PB0 never toggled")
if m:
    # The constructor's duty must be live immediately, not one period late.
    check(abs(duty_pct(m) - 25.0) < 2.0, "constructor duty %.1f%% not 25%%" % duty_pct(m))
    check(abs(freq_hz(m) - 1000) < 30, "constructor freq %.0f not 1000" % freq_hz(m))

for want in (10, 50, 75, 90):
    p.duty_u16(want * 65535 // 100)
    time.sleep_ms(20)
    m = measure(PIN)
    got = duty_pct(m) if m else -1
    print("duty %2d%% -> measured %.1f%%, reported %d" % (want, got, p.duty_u16()))
    check(m is not None and abs(got - want) < 2.0, "duty %d%% measured %.1f%%" % (want, got))

p.duty_u16(32768)
for want in (50, 200, 1000, 5000, 20000):
    p.freq(want)
    time.sleep_ms(60)
    m = measure(PIN)
    got = freq_hz(m) if m else -1
    print("freq %5d -> measured %.0f Hz, reported %d, duty %.1f%%"
          % (want, got, p.freq(), duty_pct(m) if m else -1))
    check(p.freq() == want, "freq(%d) reported %d" % (want, p.freq()))
    if want <= 1000:
        check(m is not None and abs(got - want) < want * 0.03 + 2,
              "freq %d measured %.0f" % (want, got))
        # The duty must survive a frequency change rather than being rescaled away.
        check(m is not None and abs(duty_pct(m) - 50.0) < 3.0,
              "duty drifted to %.1f%% after freq(%d)" % (duty_pct(m) if m else -1, want))
    else:
        # Above a few kHz the measurement's own error dominates, so all that is
        # claimed here is that the pin is still toggling at roughly the right
        # rate. See the module docstring.
        check(m is not None and want * 0.85 < got < want * 1.5,
              "freq %d measured %.0f, far outside the measurement's own error" % (want, got))

# --- the rails ---
p.freq(1000)
p.duty_u16(0)
time.sleep_ms(20)
check(PIN.value() == 0 and measure(PIN, 2) is None, "duty 0 did not park the pin low")
p.duty_u16(65535)
time.sleep_ms(20)
check(PIN.value() == 1 and measure(PIN, 2) is None, "duty 65535 did not park the pin high")
print("rails ok")

# --- duty_ns ---
p.freq(1000)                      # 1 ms period
p.duty_ns(250000)
time.sleep_ms(20)
m = measure(PIN)
print("duty_ns 250000 -> %s, reported %d ns" % (m, p.duty_ns()))
check(m is not None and abs(duty_pct(m) - 25.0) < 2.0, "duty_ns gave %.1f%%" % (duty_pct(m) if m else -1))
check(abs(p.duty_ns() - 250000) < 5000, "duty_ns reported %d" % p.duty_ns())
try:
    p.duty_ns(2000000)            # twice the period
    fail.append("duty_ns longer than the period was accepted")
except ValueError:
    pass

# --- two channels on one timer share a frequency but keep their own duty ---
# PC6 is TIM3_CH1, the same timer as PB0's TIM3_CH3.
p.freq(1000)
p.duty_u16(16384)
q = PWM(Pin.cpu.PC6, freq=1000, duty_u16=49152)
print("second channel:", q)
check("timer=3" in repr(q), "PC6 did not join PB0's timer: %s" % q)
time.sleep_ms(20)
mp_, mq = measure(Pin.cpu.PB0), measure(Pin.cpu.PC6)
print("PB0 %s  PC6 %s" % (mp_, mq))
check(mp_ is not None and abs(duty_pct(mp_) - 25.0) < 2.0, "shared timer moved PB0's duty")
check(mq is not None and abs(duty_pct(mq) - 75.0) < 2.0, "PC6 duty wrong")
q.freq(2000)                      # drags PB0 with it, by design
time.sleep_ms(20)
mp_, mq = measure(Pin.cpu.PB0), measure(Pin.cpu.PC6)
print("after q.freq(2000): PB0 %s  PC6 %s" % (mp_, mq))
check(p.freq() == 2000, "PB0's timer did not follow to 2000")
check(mp_ is not None and abs(duty_pct(mp_) - 25.0) < 4.0,
      "PB0 duty %.1f%% not preserved across a freq change" % (duty_pct(mp_) if mp_ else -1))
check(mq is not None and abs(duty_pct(mq) - 75.0) < 4.0,
      "PC6 duty %.1f%% not preserved" % (duty_pct(mq) if mq else -1))

# --- invert ---
r = PWM(Pin.cpu.PB10, freq=1000, duty_u16=16384, invert=True)
time.sleep_ms(20)
m = measure(Pin.cpu.PB10)
print("inverted 25%% ->", m, "%.1f%%" % (duty_pct(m) if m else -1))
check(m is not None and abs(duty_pct(m) - 75.0) < 2.0,
      "invert gave %.1f%%, expected 75%%" % (duty_pct(m) if m else -1))
r.deinit()

# --- a complementary output, which is the only PWM some pins have ---
n = PWM(Pin.cpu.PE8, freq=1000, duty_u16=16384)
print("complementary:", n)
time.sleep_ms(20)
m = measure(Pin.cpu.PE8)
print("PE8 (TIM1_CH1N) 25%% ->", m, "%.1f%%" % (duty_pct(m) if m else -1))
check(m is not None and abs(duty_pct(m) - 25.0) < 2.0,
      "CHxN gave %.1f%%, expected 25%%" % (duty_pct(m) if m else -1))
n.deinit()

# --- deinit stops the output and hands the pin back ---
p.deinit()
time.sleep_ms(20)
check(measure(Pin.cpu.PB0, 2) is None, "deinit left PB0 toggling")
try:
    p.freq()
    fail.append("freq() worked on a deinitialised PWM")
except ValueError:
    pass
q.deinit()

# --- picking a timer, and refusing the impossible ---
t = PWM(Pin.cpu.PB0, freq=1000, timer=3)
check("timer=3" in repr(t), "timer=3 not honoured: %s" % t)
t.deinit()
try:
    PWM(Pin.cpu.PB0, freq=1000, timer=2)      # PB0 has no TIM2 channel at all
    fail.append("timer=2 accepted on a pin that cannot reach it")
except ValueError:
    pass
# It can reach TIM1, though -- as a complementary output.
t = PWM(Pin.cpu.PB0, freq=1000, timer=1)
check("timer=1" in repr(t) and "N" in repr(t), "PB0 on TIM1 should be a CHxN: %s" % t)
t.deinit()
try:
    PWM(Pin.cpu.PC0)                          # ETR and BKIN only, no output compare
    fail.append("a pin with no PWM was accepted")
except ValueError:
    pass
try:
    PWM(Pin.cpu.PB0, freq=0)
    fail.append("freq=0 was accepted")
except ValueError:
    pass
try:
    PWM(Pin.cpu.PB0, freq=1000, duty_u16=70000)
    fail.append("duty_u16 over 65535 was accepted")
except ValueError:
    pass
print("argument checking ok")

# --- every pin in the table can at least be claimed and released ---
claimed = 0
for port in "ABCDEF":
    for num in range(16):
        name = "P%s%d" % (port, num)
        if name in ("PA9", "PA10", "PA11", "PA12", "PB8", "PB9", "PB5",
                    "PA4", "PA5", "PA6", "PA7", "PB6", "PB7"):
            continue          # REPL, USB, the debug probe, a sensor or the I2C bus
        try:
            pin = Pin(name)
        except (ValueError, KeyError):
            continue
        try:
            w = PWM(pin, freq=1000, duty_u16=32768)
        except ValueError:
            continue
        claimed += 1
        w.deinit()
print("pins that accepted a PWM:", claimed)
check(claimed >= 50, "only %d pins accepted a PWM" % claimed)

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
        out = pyb.exec_(DEVICE_TEST, timeout=180).decode()
    finally:
        pyb.exit_raw_repl()
        pyb.close()
    sys.stdout.write(out)
    if "FAILURES: []" not in out:
        raise SystemExit("PWM FAIL")
    print("PWM PASS")


if __name__ == "__main__":
    main()
