# machine.Encoder: quadrature decoding in the timer.
#
# No wiring needed, for the same reason test_counter.py needs none: the timer
# samples the pads, so driving PA6 and PA7 as GPIO outputs through a quadrature
# sequence is decoded exactly as a real encoder would be.
#
# Note the order -- Encoder() reconfigures both pins as floating inputs, so the
# Pin(OUT) pair has to come *after* it. Setting them up first leaves the pads
# undriven and every count reads zero, which looks like a broken decoder.
#
# Run on the target: python scripts/install_file.py ports/ch32/test_encoder.py
import machine

BSHR = 0x40010810  # GPIOA; not the STM32 address, see the port README
A_SET, B_SET = 1 << 6, 1 << 7  # PA6 = TIM3_CH1, PA7 = TIM3_CH2
A_CLR, B_CLR = 1 << 22, 1 << 23

passed = 0
failed = 0


def check(name, cond):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print("FAIL", name)


def phases():
    """Take PA6/PA7 back as outputs; Encoder() leaves them floating inputs."""
    return machine.Pin("PA6", machine.Pin.OUT), machine.Pin("PA7", machine.Pin.OUT)


def quad(n, forward=True):
    """n whole quadrature cycles: 00 -> 10 -> 11 -> 01 -> 00, or reversed."""
    seq = (
        ((A_SET, 0), (A_SET, B_SET), (0, B_SET), (0, 0))
        if forward
        else ((0, B_SET), (A_SET, B_SET), (A_SET, 0), (0, 0))
    )
    for _ in range(n):
        for a, b in seq:
            machine.mem32[BSHR] = (a or A_CLR) | (b or B_CLR)


def idle():
    machine.mem32[BSHR] = A_CLR | B_CLR


print("Encoder attrs", sorted(n for n in dir(machine.Encoder) if not n.startswith("_")))

# --- construction and defaults
e = machine.Encoder(3)
print("default pins:", e)
check(
    "Encoder(3) defaults to its channel 1 and 2 pins", "Pin(6)" in repr(e) and "Pin(7)" in repr(e)
)
check("defaults to 4x decoding", "phases=4" in repr(e))
check("starts at zero", e.value() == 0)

# --- direction.
#
# The whole point of quadrature: the same two edges mean forward or backward
# depending on which phase led. Four counts per cycle at 4x.
pa, pb = phases()
for n in (1, 10, 100):
    idle()
    e.value(0)
    quad(n)
    fwd = e.value()
    e.value(0)
    quad(n, forward=False)
    rev = e.value()
    print("  %3d cycles: forward %5d, backward %5d" % (n, fwd, rev))
    check("%d cycles forward" % n, fwd == 4 * n)
    check("%d cycles backward" % n, rev == -4 * n)

# Turning one way and then back must come out where it started.
idle()
e.value(0)
quad(50)
quad(50, forward=False)
check("there and back again is zero", e.value() == 0)

# --- value()
e.value(1000)
check("value() sets", e.value() == 1000)
quad(5)
check("counting continues from the set value", e.value() == 1020)
e.deinit()

# --- 2x decoding
e = machine.Encoder(3, phases=2)
pa, pb = phases()
idle()
e.value(0)
quad(10)
got = e.value()
print("  phases=2: 10 cycles ->", got)
check("2x decoding counts half as often", got == 20)
e.deinit()

# --- the counter wraps.
#
# 16 bits of hardware counter and the wraps counted in the update interrupt,
# with the direction read at the moment of the wrap rather than assumed -- so
# winding past the top and back down again has to land where it started.
e = machine.Encoder(3)
pa, pb = phases()
idle()
e.value(65500)
quad(100)
check("counts past 65535", e.value() == 65900)
quad(100, forward=False)
check("unwinds back through the wrap", e.value() == 65500)
e.deinit()

# --- explicit pins
e = machine.Encoder(3, machine.Pin("PA6"), machine.Pin("PA7"))
check("explicit phase pins accepted", "Pin(6)" in repr(e))
e.deinit()

# --- sharing the timer
e = machine.Encoder(3)
try:
    machine.Timer(3, freq=10)
    check("Encoder blocks a Timer on its timer", False)
except ValueError as err:
    check("Encoder blocks a Timer on its timer", "Encoder" in str(err))
try:
    machine.PWM(machine.Pin("PA6"), freq=1000, duty_u16=32768, timer=3)
    check("Encoder blocks a PWM on its timer", False)
except ValueError:
    check("Encoder blocks a PWM on its timer", True)
e.deinit()

c = machine.Counter(2)
try:
    machine.Encoder(2)
    check("Counter blocks an Encoder on its timer", False)
except ValueError as err:
    check("Counter blocks an Encoder on its timer", "Counter" in str(err))
c.deinit()

# --- errors
for bad in (6, 7):
    try:
        machine.Encoder(bad)
        check("Encoder(%d) rejected -- no channels" % bad, False)
    except ValueError:
        check("Encoder(%d) rejected -- no channels" % bad, True)

try:
    machine.Encoder(3, phases=1)
    check("phases=1 rejected", False)
except ValueError:
    check("phases=1 rejected", True)

try:
    machine.Encoder(3, machine.Pin("PB0"), machine.Pin("PA7"))
    check("a pin that is not channel 1 is rejected", False)
except ValueError:
    check("a pin that is not channel 1 is rejected", True)

machine.Pin("PA6", machine.Pin.IN)
machine.Pin("PA7", machine.Pin.IN)
print("%u passed, %u failed" % (passed, failed))
