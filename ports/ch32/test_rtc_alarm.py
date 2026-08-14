# machine.RTC alarms.
#
# There is one comparator on this RTC and it compares whole seconds against the
# same counter time.time() reads, so everything here is that fact worked
# through: ALARM0 is the only id, an interval rounds up to the next second, and
# repeat is the handler re-arming rather than anything the hardware does.
#
# Takes about half a minute, most of it waiting for alarms to come round.
#
# Run on the target: python scripts/install_file.py ports/ch32/test_rtc_alarm.py
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


rtc = machine.RTC()
print("source", rtc.source())
check("clock is running", rtc.source() != 0)

check("ALARM0 exists", machine.RTC.ALARM0 == 0)

# --- one-shot, through the irq handler ---
fired = []


def on_alarm(source):
    fired.append(time.ticks_ms())


irq = rtc.irq(trigger=machine.RTC.ALARM0, handler=on_alarm)
check("irq() returns an object", irq is not None)

rtc.alarm(machine.RTC.ALARM0, 2000)
left = rtc.alarm_left()
# Whole seconds only, and the alarm was armed against a counter that may have
# been most of the way through the current one, so 1000..2000 are both right.
check("alarm_left is in range", 1000 <= left <= 2000)
print("alarm_left", left)

t0 = time.ticks_ms()
while not fired and time.ticks_diff(time.ticks_ms(), t0) < 5000:
    time.sleep_ms(20)
check("one-shot fired", len(fired) == 1)
if fired:
    dt = time.ticks_diff(fired[0], t0)
    print("fired after", dt, "ms")
    check("fired at about the right time", 500 < dt < 3000)

# A one-shot must not come round again.
time.sleep_ms(2500)
check("one-shot did not repeat", len(fired) == 1)
check("alarm_left is 0 once fired", rtc.alarm_left() == 0)

# --- repeating ---
del fired[:]
rtc.alarm(machine.RTC.ALARM0, 1000, repeat=True)
t0 = time.ticks_ms()
while len(fired) < 3 and time.ticks_diff(time.ticks_ms(), t0) < 8000:
    time.sleep_ms(20)
check("repeat fired three times", len(fired) >= 3)
if len(fired) >= 3:
    gaps = [time.ticks_diff(fired[i + 1], fired[i]) for i in range(len(fired) - 1)]
    print("gaps", gaps)
    # Re-armed from the previous target rather than from the counter, so the
    # period must not drift even though the handler takes time to run.
    check("repeat period is one second", all(900 < g < 1100 for g in gaps))

rtc.alarm_cancel()
n = len(fired)
time.sleep_ms(2500)
check("cancel stops the repeat", len(fired) == n)

# --- absolute datetime ---
del fired[:]
now = list(rtc.datetime())
# Two seconds ahead, without carrying into the next minute: a tuple that has to
# normalise is a different test and this one is about the comparator.
if now[6] < 55:
    now[6] += 2
    rtc.alarm(machine.RTC.ALARM0, tuple(now))
    left = rtc.alarm_left()
    print("datetime alarm_left", left)
    check("datetime alarm is armed", 0 < left <= 3000)
    t0 = time.ticks_ms()
    while not fired and time.ticks_diff(time.ticks_ms(), t0) < 5000:
        time.sleep_ms(20)
    check("datetime alarm fired", len(fired) == 1)
    rtc.alarm_cancel()
else:
    print("  SKIP  datetime alarm -- too close to the minute boundary")

# --- errors ---
try:
    rtc.alarm(1, 1000)
    check("second alarm id rejected", False)
except OSError:
    check("second alarm id rejected", True)

try:
    rtc.alarm(machine.RTC.ALARM0, 0)
    check("zero interval rejected", False)
except ValueError:
    check("zero interval rejected", True)

try:
    past = list(rtc.datetime())
    past[0] -= 1
    rtc.alarm(machine.RTC.ALARM0, tuple(past))
    check("past datetime rejected", False)
except ValueError:
    check("past datetime rejected", True)

try:
    rtc.alarm(machine.RTC.ALARM0, 1000, repeat=True)
    rtc.alarm_cancel()
    rtc.alarm(machine.RTC.ALARM0, tuple(rtc.datetime()), repeat=True)
    check("repeat with a datetime rejected", False)
except ValueError:
    check("repeat with a datetime rejected", True)

# Leave nothing armed: the alarm survives a soft reset in the backup domain.
rtc.alarm_cancel()
rtc.irq(trigger=machine.RTC.ALARM0, handler=None)
check("alarm_left is 0 after cancel", rtc.alarm_left() == 0)

print("%u passed, %u failed" % (passed, failed))
