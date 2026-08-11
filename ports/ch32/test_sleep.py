"""machine.lightsleep() and machine.deepsleep() on the CH32H417.

deepsleep cannot be tested from a single script run, because it never returns
-- the chip resets on wake, and the script goes with it. So this file only
covers lightsleep and the pieces of deepsleep that can be checked without
entering Stop; scripts/test_sleep.py on the host drives the reset half, since
it survives the reboot and can time it against the host clock.

Run on the target: python scripts/install_file.py ports/ch32/test_sleep.py /hwtest/test_sleep.py
"""

import machine
import time

passed = 0
failed = 0


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print("FAIL", name, detail)


# --- lightsleep must sleep for about the right time and then RESUME.
for ms in (50, 250, 1000):
    t0 = time.ticks_ms()
    machine.lightsleep(ms)
    dt = time.ticks_diff(time.ticks_ms(), t0)
    # Generous upper bound: any interrupt ends a Sleep early by design, and USB
    # is being serviced throughout.
    check("lightsleep(%d) duration" % ms, ms - 2 <= dt <= ms + 60, "got %d ms" % dt)

check("lightsleep resumes execution", True)

# A bare lightsleep() waits for any interrupt. SysTick fires every 1 ms, so it
# returns almost immediately -- the point is only that it returns at all.
t0 = time.ticks_ms()
machine.lightsleep()
check("bare lightsleep returns", time.ticks_diff(time.ticks_ms(), t0) < 50)

# --- lightsleep must not disturb the timebase. If SLEEPDEEP were left set, the
# chip would stop here instead of the core, and this would never come back.
t0 = time.ticks_ms()
machine.lightsleep(100)
time.sleep_ms(100)
machine.lightsleep(100)
dt = time.ticks_diff(time.ticks_ms(), t0)
check("timebase survives repeated lightsleep", 290 <= dt <= 400, "got %d ms" % dt)

# --- the RTC keeps time across a lightsleep, since nothing stops in Sleep mode.
if machine.RTC().source() != 0:
    t0 = time.time()
    machine.lightsleep(2200)
    check("RTC advances across lightsleep", time.time() - t0 >= 2)

# --- deepsleep argument validation happens before anything is armed, so these
# raise rather than stopping the chip.
try:
    machine.deepsleep("soon")
    check("deepsleep rejects non-integer", False)
except (TypeError, ValueError):
    check("deepsleep rejects non-integer", True)

# --- constants the API is documented to expose.
check("DEEPSLEEP_RESET exists", hasattr(machine, "DEEPSLEEP_RESET"))
check("reset_cause is an int", isinstance(machine.reset_cause(), int))
check(
    "reset_cause is a known value",
    machine.reset_cause()
    in (
        machine.PWRON_RESET,
        machine.HARD_RESET,
        machine.WDT_RESET,
        machine.SOFT_RESET,
        machine.DEEPSLEEP_RESET,
    ),
    "got %d" % machine.reset_cause(),
)

print("PASS", passed, "FAIL", failed)
print("RESULT", "OK" if failed == 0 else "FAILED")
