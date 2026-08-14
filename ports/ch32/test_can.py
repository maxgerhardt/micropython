# machine.CAN on the bxCAN controllers.
#
# Almost all of this runs in loopback, which needs no transceiver and no bus:
# the controller acknowledges its own frames internally, so send() completes
# and recv() returns what was sent. That covers the mailboxes, the identifier
# packing, the filters, the interrupts and the state machine.
#
# What loopback cannot check is the *bit rate*, because transmitter and
# receiver share one clock and stay consistent with each other whatever it is
# set to -- a bit time built from the 400 MHz core clock instead of the 100 MHz
# bus clock would look perfect here and be four times too fast on a real bus.
# So the timing is checked the only way it can be from inside: by counting how
# long a burst of frames takes against the millisecond clock.
#
# For a real bus, see the two-node section at the end of README.md.
#
# Run on the target: python scripts/install_file.py ports/ch32/test_can.py
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


def drain(can, timeout_ms=200):
    """Wait for one frame and return it, or None if none arrives.

    recv() *returns* None on an empty FIFO rather than raising, so this polls
    on the value; treating it as an exception silently made every check here
    read the previous test's frame.
    """
    t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < timeout_ms:
        msg = can.recv()
        if msg is not None:
            return msg
    return None


print("CAN attrs", sorted(n for n in dir(machine.CAN) if not n.startswith("_")))
check("three controllers", machine.CAN.TX_QUEUE_LEN == 3)

# --- construction and modes
can = machine.CAN(1, bitrate=500000, mode=machine.CAN.MODE_LOOPBACK)
check("state is active", can.state() == machine.CAN.STATE_ACTIVE)

t = can.get_timings()
print("timings", t)
# get_timings() returns (bitrate, sample_point, sjw, tseg1, tseg2, ...).
check("bitrate is what was asked for", t[0] == 500000)

# --- a standard frame round trip
can.send(0x123, b"\x01\x02\x03\x04")
msg = drain(can)
check("standard frame arrives", msg is not None)
if msg:
    print("recv", msg)
    check("standard id", msg[0] == 0x123)
    check("standard data", bytes(msg[1]) == b"\x01\x02\x03\x04")
    check("no extended flag", not (msg[2] & machine.CAN.FLAG_EXT_ID))

# --- an extended frame, and a zero-length one
can.send(0x1ABCDEF, b"\xaa\xbb", flags=machine.CAN.FLAG_EXT_ID)
msg = drain(can)
check("extended frame arrives", msg is not None)
if msg:
    check("extended id", msg[0] == 0x1ABCDEF)
    check("extended flag set", bool(msg[2] & machine.CAN.FLAG_EXT_ID))

can.send(0x001, b"")
msg = drain(can)
check("empty frame arrives", msg is not None and len(msg[1]) == 0)

# Eight bytes is the maximum for classic CAN; nine must be refused.
can.send(0x7FF, b"01234567")
msg = drain(can)
check("eight byte frame", msg is not None and bytes(msg[1]) == b"01234567")
try:
    can.send(0x7FF, b"012345678")
    check("nine bytes rejected", False)
except (ValueError, OSError, OverflowError):
    check("nine bytes rejected", True)

# --- three mailboxes, filled before any drains
idx = [can.send(0x200 + i, bytes([i])) for i in range(3)]
print("mailbox indexes", idx)
check("send returns distinct mailboxes", len(set(idx)) == 3)
got = sorted(m[0] for m in (drain(can), drain(can), drain(can)) if m)
check("all three arrive", got == [0x200, 0x201, 0x202])

# --- filters
can.set_filters([(0x300, 0x7FF, 0)])
can.send(0x300, b"\x01")
check("matching id passes the filter", drain(can) is not None)
can.send(0x301, b"\x02")
check("other id is filtered out", drain(can, 100) is None)

# Filters are (id, mask, flags) triples.
# A mask that ignores the low bits takes a range.
can.set_filters([(0x300, 0x7F0, 0)])
can.send(0x305, b"\x03")
check("masked range passes", drain(can) is not None)

# No filters at all means accept everything, not reject everything.
can.set_filters([])
can.send(0x555, b"\x04")
check("empty filter list accepts all", drain(can) is not None)

# --- counters
c = can.get_counters()
print("counters", c)
check("no transmit errors in loopback", c[0] == 0)
check("no receive errors in loopback", c[1] == 0)
check("never went bus-off", c[4] == 0)

# --- interrupt
seen = []


def on_can(obj):
    seen.append(obj.irq().flags())


irq = can.irq(handler=on_can, trigger=machine.CAN.IRQ_RX)
can.send(0x111, b"\x09")
t0 = time.ticks_ms()
while not seen and time.ticks_diff(time.ticks_ms(), t0) < 500:
    pass
check("rx interrupt fires", len(seen) > 0)
if seen:
    check("rx flag is set", bool(seen[0] & machine.CAN.IRQ_RX))
can.irq(handler=None, trigger=0)
drain(can)


# --- bit rate, the one thing loopback alone cannot verify.
#
# A standard frame with 8 data bytes is 44 + 64 bits plus stuffing, call it
# 120 on the wire. At 125000 bit/s that is 960 us; at 500000 it is 240. Send a
# burst with the mailboxes kept full and time it: if the bit time were built
# from the 400 MHz core clock instead of the 100 MHz bus clock this comes out
# four times too quick.
def burst_us(bitrate, count=40):
    """Microseconds a frame, measured by counting frames actually received.

    Counting sends would measure the Python loop instead: send() reports a full
    mailbox by raising, and three mailboxes accept three frames instantly while
    the wire is still busy with the first.
    """
    c = machine.CAN(1, bitrate=bitrate, mode=machine.CAN.MODE_LOOPBACK)
    payload = b"Z" * 8
    got = 0
    t0 = time.ticks_us()
    while got < count:
        try:
            c.send(0x123, payload)
        except OSError:
            pass  # every mailbox busy, which is the point of the burst
        if c.recv() is not None:
            got += 1
    dt = time.ticks_diff(time.ticks_us(), t0)
    c.deinit()
    return dt / count


for bitrate, nominal_us in ((125000, 960), (500000, 240)):
    per = burst_us(bitrate)
    print("  %d bit/s: %.0f us a frame, about %d expected" % (bitrate, per, nominal_us))
    # Generous: the Python loop around each frame costs real time too, and only
    # a gross error -- a factor of four -- is being looked for.
    check("%d bit/s frame time" % bitrate, nominal_us * 0.7 < per < nominal_us * 3)

# --- the other controllers exist and are independent
can.deinit()
for n in (1, 2, 3):
    c = machine.CAN(n, bitrate=250000, mode=machine.CAN.MODE_LOOPBACK)
    c.send(0x10 + n, bytes([n]))
    m = drain(c)
    check("CAN(%d) loops back" % n, m is not None and m[0] == 0x10 + n)
    c.deinit()

try:
    machine.CAN(4, bitrate=250000)
    check("CAN(4) rejected", False)
except ValueError:
    check("CAN(4) rejected", True)

# --- normal mode with nobody to answer.
#
# One node on a bus, or a node with no transceiver at all: nothing
# acknowledges, so the frame is retransmitted forever and the transmit error
# counter climbs until the controller goes error-passive at 128. Textbook CAN,
# and worth pinning down because it is also what a wrong pin assignment or a
# dead transceiver looks like -- and because it did *not* behave this way until
# the RCC peripheral reset went into init(): a controller that had once been in
# loopback or silent mode would quietly acknowledge its own traffic instead.
c = machine.CAN(1, bitrate=125000, mode=machine.CAN.MODE_NORMAL)
c.send(0x123, bytes([1]))
time.sleep_ms(50)
counters = c.get_counters()
print("normal mode, nobody listening:", counters, "state", c.state())
check("transmit errors accumulate", counters[0] > 0)
check("frame is still pending", counters[5] > 0)
check("nothing was received", counters[6] == 0)
c.deinit()

# --- a real bus, if one is wired.
#
# CAN1 on PB7/PB6 and CAN3 on PC5/PC4, each to a 3.3 V transceiver, CANH to
# CANH and CANL to CANL with 120 ohm at each end. See README.md.
#
# This is the only thing that can check the bit timing against another
# controller rather than against itself, and the only thing that proves the
# pads drive at all. Everything above passes without it, so the section skips
# cleanly when nothing is connected.
a = machine.CAN(1, bitrate=500000, mode=machine.CAN.MODE_NORMAL)
b = machine.CAN(3, bitrate=500000, mode=machine.CAN.MODE_NORMAL)
a.send(0x100, b"\xaa\x55")
probe = drain(b, 300)

if probe is None:
    print("  SKIP  two-node bus -- nothing received on CAN3, is it wired?")
    a.deinit()
    b.deinit()
else:
    check("bus: frame crosses to the other controller", probe[0] == 0x100)
    check("bus: payload survives", bytes(probe[1]) == b"\xaa\x55")

    # Back the other way, so both transmitters and both receivers are covered.
    b.send(0x321, b"wxyz")
    m = drain(a)
    check("bus: reverse direction", m is not None and m[0] == 0x321)

    check("bus: no transmit errors", a.get_counters()[0] == 0 and b.get_counters()[0] == 0)
    check("bus: no receive errors", a.get_counters()[1] == 0 and b.get_counters()[1] == 0)

    a.send(0x1ABCDEF, b"12345678", flags=machine.CAN.FLAG_EXT_ID)
    m = drain(b)
    check(
        "bus: extended id and eight bytes",
        m is not None and m[0] == 0x1ABCDEF and bytes(m[1]) == b"12345678",
    )

    # A filter on the receiver. This is what caught CAN3 taking its filter
    # scale from the wrong register: an accept-everything filter still worked,
    # and any real (id, mask) pair matched nothing.
    b.set_filters([(0x200, 0x7FF, 0)])
    a.send(0x200, b"\x01")
    check("bus: filter accepts a match", drain(b) is not None)
    a.send(0x201, b"\x02")
    check("bus: filter rejects the rest", drain(b, 200) is None)
    b.set_filters([])

    seen = []
    b.irq(handler=lambda o: seen.append(o.irq().flags()), trigger=machine.CAN.IRQ_RX)
    a.send(0x300, b"\xff")
    t0 = time.ticks_ms()
    while not seen and time.ticks_diff(time.ticks_ms(), t0) < 500:
        pass
    check("bus: receive interrupt", len(seen) > 0 and (seen[0] & machine.CAN.IRQ_RX))
    b.irq(handler=None, trigger=0)
    drain(b)
    a.deinit()
    b.deinit()

    # Every standard bit rate, both controllers agreeing on the sample point.
    # Nothing gets through at all if the bit time is wrong, which makes this a
    # far stronger check than the loopback burst above.
    for rate in (125000, 250000, 500000, 800000, 1000000):
        a = machine.CAN(1, bitrate=rate, mode=machine.CAN.MODE_NORMAL)
        b = machine.CAN(3, bitrate=rate, mode=machine.CAN.MODE_NORMAL)
        a.send(0x100, b"\xaa\x55")
        m = drain(b)
        ok = m is not None and m[0] == 0x100 and bytes(m[1]) == b"\xaa\x55"
        print("  %7d bit/s %s" % (rate, "ok" if ok else "FAILED"))
        check("bus at %d bit/s" % rate, ok)
        check("bus at %d bit/s is error free" % rate, a.get_counters()[0] == 0)
        a.deinit()
        b.deinit()

print("%u passed, %u failed" % (passed, failed))
