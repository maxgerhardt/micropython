"""Verify machine.Pin against a PB0-PB1 loopback wire.

The board under test has PB0 shorted to PB1. That makes the two pins each
other's instrument: whatever one drives, the other observes. It also means the
two must never be outputs at the same time -- opposing drivers on a shorted
pair is a dead short through both pads -- so every phase below starts from
park(), which returns both ends to high-impedance inputs first.
"""

import sys, os

sys.path.insert(
    0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "scripts")
)
from serial_expect import DEFAULT_PORT

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import pyboard

DEVICE_TEST = """
import time
from machine import Pin, Signal

A = Pin.cpu.PB0
B = Pin.cpu.PB1

def park():
    # Both ends high-impedance before anything else changes direction.
    A.init(Pin.IN)
    B.init(Pin.IN)

fail = []

def check(name, got, want):
    if got != want:
        fail.append("%s: got %r want %r" % (name, got, want))

# --- naming: the three spellings must be the same object ---
park()
check("str id", Pin("PB0") is A, True)
check("int id", Pin(16) is A, True)
check("int id B", Pin(17) is B, True)
check("repr", repr(A), "Pin(PB0)")

# --- every pin the package bonds out is addressable ---
n = 0
for port in range(6):
    for num in range(16):
        if port == 5 and num == 15:
            continue  # PF15 does not exist on the die
        Pin(port * 16 + num)  # no mode argument: names the pin, touches nothing
        n += 1
check("pin count", n, 95)
try:
    Pin(5 * 16 + 15)
    fail.append("PF15 should not be constructible")
except ValueError:
    pass

# --- continuity, proven without driving anything ---
# A pull is the only thing on the wire, so if B follows A's pull the short is
# really there. If this fails, every later check is meaningless.
park()
A.init(Pin.IN, Pin.PULL_UP)
time.sleep_ms(2)
check("pullup continuity", B.value(), 1)
A.init(Pin.IN, Pin.PULL_DOWN)
time.sleep_ms(2)
check("pulldown continuity", B.value(), 0)
check("pull readback", A.pull(), Pin.PULL_DOWN)

# --- A drives, B observes ---
park()
B.init(Pin.IN)
A.init(Pin.OUT)
A.off()
check("A low -> B", B.value(), 0)
check("A low readback", A.value(), 0)
A.on()
check("A high -> B", B.value(), 1)
check("A high readback", A.value(), 1)
A.toggle()
check("A toggle -> B", B.value(), 0)
A(1)
check("A call -> B", B.value(), 1)

# --- the same in reverse ---
park()
A.init(Pin.IN)
B.init(Pin.OUT, value=0)
check("B low -> A", A.value(), 0)
B.on()
check("B high -> A", A.value(), 1)

# --- a driven pin beats a pull on the other end ---
park()
B.init(Pin.IN, Pin.PULL_UP)
A.init(Pin.OUT, value=0)
check("drive beats pullup", B.value(), 0)

# --- reading an input must not report the pull as the pin level ---
# The latch selects the pull direction on this silicon, so reading OUTDR for an
# input would always return 1 here regardless of what the wire is doing.
park()
A.init(Pin.OUT, value=0)
B.init(Pin.IN, Pin.PULL_UP)
check("input reads wire not latch", B.value(), 0)

# --- open drain: pulls low, releases high ---
park()
B.init(Pin.IN, Pin.PULL_UP)
A.init(Pin.OPEN_DRAIN)
A.off()
check("od low", B.value(), 0)
A.on()
time.sleep_ms(2)
check("od released", B.value(), 1)
check("od mode readback", A.mode(), Pin.OPEN_DRAIN)

# --- mode and drive readback ---
park()
A.init(Pin.OUT)
check("mode out", A.mode(), Pin.OUT)
A.init(Pin.IN)
check("mode in", A.mode(), Pin.IN)
check("no pull", A.pull(), None)
A.init(Pin.ANALOG)
check("mode analog", A.mode(), Pin.ANALOG)
park()
A.init(Pin.OUT, drive=Pin.DRIVE_3)
check("drive readback", A.drive(), Pin.DRIVE_3)
A.drive(Pin.DRIVE_0)
check("drive set", A.drive(), Pin.DRIVE_0)

# --- Signal rides on the pin protocol ---
park()
B.init(Pin.IN)
A.init(Pin.OUT)
sig = Signal(A, invert=True)
sig.on()
check("signal on inverted", B.value(), 0)
sig.off()
check("signal off inverted", B.value(), 1)

# --- interrupts ---
park()
A.init(Pin.OUT, value=0)
B.init(Pin.IN)

hits = []
B.irq(lambda p: hits.append(p), Pin.IRQ_RISING)
A.on()
time.sleep_ms(20)
check("rising count", len(hits), 1)
check("rising arg", hits[0] is B, True)
A.off()
time.sleep_ms(20)
check("no falling when rising-only", len(hits), 1)

hits2 = []
B.irq(lambda p: hits2.append(p), Pin.IRQ_FALLING)
A.on()
time.sleep_ms(20)
check("no rising when falling-only", len(hits2), 0)
A.off()
time.sleep_ms(20)
check("falling count", len(hits2), 1)

# EXTI line N is shared by pin N of every port, so PA1 cannot take line 1
# while PB1 holds it. Silently stealing the line would be the worst outcome.
try:
    Pin.cpu.PA1.irq(lambda p: None, Pin.IRQ_RISING)
    fail.append("EXTI conflict not detected")
except ValueError:
    pass

B.irq(None)
# After releasing, another port may claim the line.
Pin.cpu.PA1.irq(lambda p: None, Pin.IRQ_RISING)
Pin.cpu.PA1.irq(None)

# A hard handler runs inside the ISR with the heap locked, so it must not
# allocate: count into a preallocated bytearray rather than appending to a list.
park()
A.init(Pin.OUT, value=0)
B.init(Pin.IN)
counter = bytearray(1)

def hard_handler(p):
    counter[0] += 1

B.irq(hard_handler, Pin.IRQ_RISING, hard=True)
A.on()
# No scheduler round trip to wait for: a hard handler has already run by the
# time the edge is over.
check("hard irq", counter[0], 1)
B.irq(None)

hits3 = []
B.irq(lambda p: hits3.append(1), Pin.IRQ_RISING | Pin.IRQ_FALLING)
A.on()
time.sleep_ms(20)
A.off()
time.sleep_ms(20)
check("both edges", len(hits3), 2)
B.irq(None)

park()
print("FAILURES:", fail)
"""


ARM_IRQ = """
from machine import Pin
Pin.cpu.PB1.irq(lambda p: None, Pin.IRQ_RISING)
print("ARMED")
"""

# PA1 shares EXTI line 1 with PB1. If the soft reset did not release the line,
# claiming it here raises "EXTI line in use by another port" -- and the handler
# the line still points at was thrown away with the old heap.
CLAIM_AFTER_RESET = """
from machine import Pin
Pin.cpu.PA1.irq(lambda p: None, Pin.IRQ_RISING)
Pin.cpu.PA1.irq(None)
print("RELEASED")
"""


def run(port, script):
    # Pyboard.enter_raw_repl() soft-resets the board on the way in, which is
    # exactly the transition being tested here.
    pyb = pyboard.Pyboard(port, 115200)
    pyb.enter_raw_repl()
    try:
        return pyb.exec_(script).decode()
    finally:
        pyb.exit_raw_repl()
        pyb.close()


def main():
    port = os.environ.get("MPY_PORT", DEFAULT_PORT)

    out = run(port, DEVICE_TEST)
    sys.stdout.write(out)
    if "FAILURES: []" not in out:
        raise SystemExit("GPIO FAIL")

    if "ARMED" not in run(port, ARM_IRQ):
        raise SystemExit("GPIO FAIL: could not arm interrupt")
    if "RELEASED" not in run(port, CLAIM_AFTER_RESET):
        raise SystemExit("GPIO FAIL: soft reset did not release the EXTI line")

    print("GPIO PASS")


if __name__ == "__main__":
    main()
