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

# --- normal mode with nothing on the bus.
#
# This does not do what a bxCAN datasheet would lead you to expect. With no
# transceiver and no second node there is nobody to acknowledge, so the frame
# should be retransmitted forever and the transmit error counter should climb.
# Instead it completes, is acknowledged, and comes back in the receive FIFO --
# and it still does with the RX pin taken back as a plain GPIO input, which
# means the receiver is not being fed from the pad at all.
#
# So this pins down what the silicon actually does rather than what it ought
# to: no error, and the controller stays error-active. Whether the pads are
# driving at all is the one thing loopback and a single node cannot answer;
# the two-node wiring in README.md settles it.
c = machine.CAN(1, bitrate=125000, mode=machine.CAN.MODE_NORMAL)
c.send(0x123, bytes([1]))
time.sleep_ms(50)
counters = c.get_counters()
print("normal mode, no bus:", counters, "state", c.state())
check("no transmit errors", counters[0] == 0)
check("controller stays error-active", c.state() == machine.CAN.STATE_ACTIVE)
check("frame did not stay stuck in a mailbox", counters[5] == 0)
c.deinit()

print("%u passed, %u failed" % (passed, failed))
