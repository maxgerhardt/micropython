# machine.Counter on a timer's ETR input.
#
# No wiring needed. The pin is its own signal source: the counter samples the
# pad, so driving PA5 as a GPIO output is counted exactly as an external signal
# would be. Note the order -- Counter() reconfigures the pin as a floating
# input, so the Pin(OUT) has to come after it, and a test that sets the pin up
# first silently counts nothing.
#
# For a real external signal there is nothing to do differently; just leave the
# pin alone and wire the source to PA5.
#
# Run on the target: python scripts/install_file.py ports/ch32/test_counter.py
import time

import machine

GPIOA_BSHR = 0x40010810  # not the STM32 address; see the port README
PA5_SET = 1 << 5
PA5_CLR = 1 << 21

passed = 0
failed = 0


def check(name, cond):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print("FAIL", name)


@micropython.viper
def pulses(n: int, hold: int):
    """n low-high-low cycles on PA5, with a hold of a few hundred ns."""
    p = ptr32(0x40010810)
    for _ in range(n):
        p[0] = 0x20
        for _ in range(hold):
            pass
        p[0] = 0x200000
        for _ in range(hold):
            pass


def driving():
    """Take PA5 back as an output; Counter() leaves it a floating input."""
    return machine.Pin("PA5", machine.Pin.OUT)


print("Counter attrs", sorted(n for n in dir(machine.Counter) if not n.startswith("_")))

# --- construction
c = machine.Counter(2)
print("default source:", c)
check("Counter(2) defaults to its ETR pin", "Pin(5)" in repr(c))
check("starts at zero", c.value() == 0)

# --- counting.
#
# Every pulse, none missed and none doubled. This is the whole point of
# external clock mode 2: the pin clocks the counter directly and no software is
# in the loop, so the count is exact by construction rather than by luck.
pin = driving()
for n in (1, 10, 1000):
    c.value(0)
    pulses(n, 3)
    got = c.value()
    print("  %5d pulses -> %d" % (n, got))
    check("%d pulses counted exactly" % n, got == n)

# --- rate.
#
# Short pulses, to show nothing is being missed when the edges come faster than
# any interrupt could keep up with.
c.value(0)
t0 = time.ticks_us()
pulses(2000, 0)
dt = time.ticks_diff(time.ticks_us(), t0)
got = c.value()
print("  2000 pulses in %d us (%d ns each) -> %d" % (dt, dt * 500, got))
check("fast pulses counted exactly", got == 2000)

# --- the 16-bit counter wraps.
#
# The hardware counter is 16 bits and the update interrupt counts the wraps, so
# anything past 65535 is the two combined. Without that this reads 4464.
c.value(0)
pulses(70000, 0)
check("counts past 65535", c.value() == 70000)

# --- value() sets as well as gets
c.value(1234)
check("value() sets", c.value() == 1234)
pulses(6, 3)
check("counting continues from the set value", c.value() == 1240)
c.value(0)
check("value(0) resets", c.value() == 0)
c.deinit()

# --- edge selection.
#
# A square wave has one edge of each kind per cycle, so counting a burst cannot
# tell the two apart. Watching *when* the count moves can: with RISING it moves
# as the line goes high, with FALLING as it comes back down.
for edge, name in ((machine.Counter.RISING, "RISING"), (machine.Counter.FALLING, "FALLING")):
    c = machine.Counter(2, edge=edge)
    pin = driving()
    pin.value(0)
    time.sleep_ms(1)
    c.value(0)
    pin.value(1)
    time.sleep_ms(1)
    on_high = c.value()
    pin.value(0)
    time.sleep_ms(1)
    on_low = c.value()
    print("  %s: after rise %d, after fall %d" % (name, on_high, on_low))
    if edge == machine.Counter.RISING:
        check("RISING counts the rise", on_high == 1 and on_low == 1)
    else:
        check("FALLING counts the fall", on_high == 0 and on_low == 1)
    c.deinit()

# --- direction
c = machine.Counter(2, direction=machine.Counter.DOWN)
pin = driving()
c.value(0)
pulses(10, 3)
got = c.value()
print("  counting down: 10 pulses ->", got)
check("DOWN counts backwards", got == -10)
c.deinit()

# --- sharing the timer.
#
# A Counter owns the whole timer, because it is the counter's clock. PWM and
# Timer have to be kept off it, and the error should say who has it.
c = machine.Counter(2)
try:
    machine.Timer(2, freq=10)
    check("Counter blocks a Timer on its timer", False)
except ValueError as e:
    check("Counter blocks a Timer on its timer", "Counter" in str(e))
try:
    machine.PWM(machine.Pin("PA5"), freq=1000, duty_u16=32768, timer=2)
    check("Counter blocks a PWM on its timer", False)
except ValueError:
    check("Counter blocks a PWM on its timer", True)
c.deinit()

# And the other way: a Timer's timer is not available to a Counter.
t = machine.Timer(2, freq=10)
try:
    machine.Counter(2)
    check("Timer blocks a Counter on its timer", False)
except ValueError as e:
    check("Timer blocks a Counter on its timer", "Timer" in str(e))
t.deinit()

# --- errors
for bad in (6, 7):
    try:
        machine.Counter(bad)
        check("Counter(%d) rejected -- no ETR" % bad, False)
    except ValueError:
        check("Counter(%d) rejected -- no ETR" % bad, True)

try:
    machine.Counter(2, machine.Pin("PA7"))
    check("a non-ETR pin is rejected", False)
except ValueError:
    check("a non-ETR pin is rejected", True)

try:
    machine.Counter(2, edge=9)
    check("bad edge rejected", False)
except ValueError:
    check("bad edge rejected", True)

machine.Pin("PA5", machine.Pin.IN)
print("%u passed, %u failed" % (passed, failed))
