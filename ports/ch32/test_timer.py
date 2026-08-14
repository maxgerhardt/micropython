# machine.Timer on the hardware timers.
#
# These are real TIMs, not soft timers, so the interesting properties are the
# ones a soft timer cannot have: the period is exact rather than rounded to a
# SysTick, the callback arrives from an interrupt, and the twelve of them have
# to be shared with machine.PWM without either side stealing the other's.
#
# Run on the target: python scripts/install_file.py ports/ch32/test_timer.py
import time

import machine

passed = 0
failed = 0


def check(name, cond):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print("FAIL", name)


def count_for(timer_args, ms):
    """Run a timer for ms and return the callback timestamps."""
    seen = []
    t = machine.Timer(-1, callback=lambda x: seen.append(time.ticks_us()), **timer_args)
    time.sleep_ms(ms)
    t.deinit()
    return seen


print("Timer attrs", sorted(n for n in dir(machine.Timer) if not n.startswith("_")))
check("ONE_SHOT and PERIODIC differ", machine.Timer.ONE_SHOT != machine.Timer.PERIODIC)

# --- allocation
#
# Timer(-1) should take TIM6 first: it and TIM7 are the two with no output
# channels, so a Timer there costs machine.PWM nothing.
t = machine.Timer(-1, freq=10)
print("auto-allocated:", t)
check("auto allocation prefers a channel-less timer", "Timer(6" in repr(t) or "Timer(7" in repr(t))
t.deinit()

t = machine.Timer(9, freq=10)
check("explicit id is honoured", "Timer(9" in repr(t))
t.deinit()

for bad in (0, 13, 99):
    try:
        machine.Timer(bad, freq=10)
        check("Timer(%d) rejected" % bad, False)
    except ValueError:
        check("Timer(%d) rejected" % bad, True)

# --- period accuracy.
#
# Measured from a *hard* callback, which runs in the interrupt itself. A soft
# callback goes through the scheduler and inherits its delays -- a garbage
# collection in the middle of the run turns a 10 ms period into gaps of 45 us
# and 26 ms, which says nothing about the timer and everything about when the
# VM got round to the callback.
#
# The stamps go into a preallocated array because a hard callback runs with the
# heap locked and must not allocate.
import array

stamps = array.array("i", [0] * 64)
n_stamps = 0


def stamp(t):
    global n_stamps
    if n_stamps < 64:
        stamps[n_stamps] = time.ticks_us()
        n_stamps += 1


for freq, ms, expect_us in ((100, 300, 10000), (1000, 60, 1000), (50, 400, 20000)):
    n_stamps = 0
    t = machine.Timer(-1, freq=freq, hard=True, callback=stamp)
    time.sleep_ms(ms)
    t.deinit()
    check("%d Hz fires" % freq, n_stamps >= 3)
    if n_stamps >= 3:
        gaps = [time.ticks_diff(stamps[i + 1], stamps[i]) for i in range(n_stamps - 1)]
        print(
            "  %4d Hz: %d callbacks, gap %d..%d us (want %d)"
            % (freq, n_stamps, min(gaps), max(gaps), expect_us)
        )
        # The mean pins the period down: the hardware cannot drift, so an
        # average that is off says the prescaler or reload was rounded. The
        # spread is a separate question -- another interrupt in progress can
        # delay entry by a microsecond or two, and ticks_us() quantises on top
        # of that -- so it gets its own, looser bound.
        mean = sum(gaps) / len(gaps)
        spread = max(abs(g - expect_us) for g in gaps)
        check("%d Hz period is exact" % freq, abs(mean - expect_us) <= 1)
        check("%d Hz jitter is bounded" % freq, spread <= 5)

# period= is in units of 1/tick_hz, so the default makes it milliseconds.
seen = count_for({"period": 25}, 200)
check("period=25 ms fires about 8 times", 6 <= len(seen) <= 9)
seen = count_for({"period": 5000, "tick_hz": 1000000}, 200)
check("tick_hz scales the period", 35 <= len(seen) <= 45)  # 5 ms -> ~40 in 200 ms

# --- one shot
seen = []
t = machine.Timer(-1, mode=machine.Timer.ONE_SHOT, period=50, callback=lambda x: seen.append(1))
time.sleep_ms(300)
check("one-shot fires exactly once", len(seen) == 1)
t.deinit()

# --- the callback gets the timer, and a hard callback runs from the interrupt
got = []
t = machine.Timer(-1, freq=200, callback=lambda x: got.append(x))
time.sleep_ms(100)
t.deinit()
check("callback receives the timer object", len(got) > 0 and got[0] is t)

# A hard callback runs in the interrupt itself, with the heap locked, so it
# must not allocate -- incrementing a slot of an existing list does not, since
# small integers are tagged rather than heap objects.
hard_count = 0


def on_hard(t):
    global hard_count
    hard_count += 1  # small integers are tagged, so this does not allocate


t = machine.Timer(-1, freq=500, hard=True, callback=on_hard)
time.sleep_ms(100)
t.deinit()
print("  hard callbacks in 100 ms at 500 Hz:", hard_count)
check("hard callback runs", hard_count > 20)

# And one that does allocate must not take the board down with it. The heap
# lock turns it into a MemoryError; the driver then disables the callback and
# hands the exception to the main thread, so it surfaces here rather than being
# printed from inside the interrupt -- which is what wedged the board while
# this was being written, by starving tud_task() until TinyUSB asserted.
t = machine.Timer(-1, freq=200, hard=True, callback=lambda x: [0] * 8)
raised = None
try:
    for _ in range(200):
        time.sleep_ms(1)
except MemoryError as e:
    raised = e
t.deinit()
check("a raising hard callback reaches the main thread", raised is not None)
check("board survived a raising hard callback", True)

# --- deinit stops it
seen = []
t = machine.Timer(-1, freq=500, callback=lambda x: seen.append(1))
time.sleep_ms(50)
t.deinit()
n = len(seen)
time.sleep_ms(100)
check("deinit stops the callbacks", len(seen) == n)
check("deinit is idempotent", t.deinit() is None)

# --- counter()
t = machine.Timer(-1, freq=10)  # 100 ms period
a = t.counter()
time.sleep_ms(20)
b = t.counter()
print("  counter %d -> %d us" % (a, b))
check("counter advances", b > a)
check("counter stays inside the period", b < 100000)
t.deinit()

# --- sharing with PWM.
#
# A Timer owns the update event and therefore the period, which is exactly what
# a PWM channel needs to control, so neither may take a timer the other holds.
pwm = machine.PWM(machine.Pin("PA6"), freq=1000, duty_u16=32768)
pwm_timer = None
for tid in range(1, 13):
    try:
        machine.Timer(tid, freq=10).deinit()
    except ValueError:
        pwm_timer = tid
print("  PWM is on timer", pwm_timer)
check("PWM blocks a Timer on its timer", pwm_timer is not None)

# And the other way round: PWM must refuse a timer a Timer is holding.
if pwm_timer is not None:
    pwm.deinit()
    t = machine.Timer(pwm_timer, freq=10)
    try:
        machine.PWM(machine.Pin("PA6"), freq=1000, duty_u16=32768, timer=pwm_timer)
        check("Timer blocks a PWM on its timer", False)
    except ValueError:
        check("Timer blocks a PWM on its timer", True)
    # Without asking for that timer specifically, PWM should simply go
    # somewhere else rather than fail.
    pwm2 = machine.PWM(machine.Pin("PA6"), freq=1000, duty_u16=32768)
    check("PWM finds another timer", pwm2 is not None)
    pwm2.deinit()
    t.deinit()
else:
    pwm.deinit()

# --- errors
# Timer(id) on its own is legal and simply does not start; init() is what
# needs a period, the same shape stm32's Timer has.
t = machine.Timer(-1)
try:
    t.init()
    check("init without a period is rejected", False)
except ValueError:
    check("init without a period is rejected", True)
t.deinit()

try:
    machine.Timer(-1, freq=0)
    check("zero freq rejected", False)
except ValueError:
    check("zero freq rejected", True)

try:
    machine.Timer(-1, mode=7, freq=10)
    check("bad mode rejected", False)
except ValueError:
    check("bad mode rejected", True)

# Longer than the 16-bit prescaler and reload can reach, about 43 s at 100 MHz.
try:
    machine.Timer(-1, period=120000).deinit()
    check("over-long period rejected", False)
except ValueError:
    check("over-long period rejected", True)

print("%u passed, %u failed" % (passed, failed))
