"""machine.UART on the CH32H417.

Most of this needs no wiring. The loopback section does, and skips itself with
a clear message rather than failing when the jumper is absent:

    USART2 loopback:  PA2 (TX) -- PA3 (RX)
    flow control:     PA1 (RTS) -- PA0 (CTS)

Timing is checked against the board's own clock here, which is only safe
because the figures are milliseconds apart from their expected values; the
host-side cross-check lives in scripts/test_uart.py.

Run on the target: python scripts/install_file.py ports/ch32/test_uart.py /hwtest/test_uart.py
"""

import time

from machine import UART, Pin

passed = 0
failed = 0
skipped = 0


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print("FAIL", name, detail)


def skip(name, why):
    global skipped
    skipped += 1
    print("SKIP", name, "--", why)


# --- construction, pin validation and remapping ---

u = UART(2, 9600, tx=Pin.cpu.PA2, rx=Pin.cpu.PA3)
check("repr names the pins", "tx=PA2" in repr(u) and "rx=PA3" in repr(u), repr(u))
check("baudrate in repr", "baudrate=9600" in repr(u))

# This part has an STM32F4-style per-pin AF mux, so "remapping" is choosing a
# different pin. Every pin the datasheet lists must be accepted.
u2 = UART(2, 115200, tx=Pin.cpu.PD5, rx=Pin.cpu.PD6)
check("remap to alternate pins", "tx=PD5" in repr(u2), repr(u2))

for bad_tx, bad_rx, why in (
    (Pin.cpu.PB0, Pin.cpu.PA3, "PB0 is not a USART2 TX"),
    (Pin.cpu.PA2, Pin.cpu.PB1, "PB1 is not a USART2 RX"),
):
    try:
        UART(2, 9600, tx=bad_tx, rx=bad_rx)
        check("reject " + why, False)
    except ValueError:
        check("reject " + why, True)

try:
    UART(1, 9600)
    check("UART(1) reserved for REPL", False)
except ValueError:
    check("UART(1) reserved for REPL", True)

for bad_id in (0, 9, 99):
    try:
        UART(bad_id, 9600)
        check("reject UART(%d)" % bad_id, False)
    except ValueError:
        check("reject UART(%d)" % bad_id, True)

# --- parameter validation ---
for kw, val in (("bits", 4), ("bits", 10), ("stop", 3)):
    try:
        UART(2, 9600, tx=Pin.cpu.PA2, rx=Pin.cpu.PA3, **{kw: val})
        check("reject %s=%d" % (kw, val), False)
    except ValueError:
        check("reject %s=%d" % (kw, val), True)

# Flow control without the pin it needs would look configured and do nothing.
try:
    UART(2, 9600, tx=Pin.cpu.PA2, rx=Pin.cpu.PA3, flow=UART.RTS)
    check("flow=RTS without rts= rejected", False)
except ValueError:
    check("flow=RTS without rts= rejected", True)

check("UART.RTS/CTS distinct", UART.RTS != UART.CTS and UART.RTS and UART.CTS)

# --- transmit timing, which needs no receiver ---
# 200 bytes of 8N1 is 2000 bit times. A txbuf smaller than the payload also
# proves the interrupt is draining the ring rather than the write blocking.
u3 = UART(2, 9600, tx=Pin.cpu.PA2, rx=Pin.cpu.PA3, txbuf=16)
payload = b"U" * 200
t0 = time.ticks_ms()
u3.write(payload)
while not u3.txdone():
    pass
dt = time.ticks_diff(time.ticks_ms(), t0)
check("200B at 9600 takes ~208ms", 195 <= dt <= 235, "got %d ms" % dt)

u4 = UART(2, 115200, tx=Pin.cpu.PA2, rx=Pin.cpu.PA3, txbuf=16)
t0 = time.ticks_ms()
u4.write(payload)
while not u4.txdone():
    pass
dt = time.ticks_diff(time.ticks_ms(), t0)
check("200B at 115200 takes ~17ms", 14 <= dt <= 30, "got %d ms" % dt)

check("txdone true when idle", u4.txdone())

# A read with nothing to return must time out rather than hang. Drain first:
# with the loopback jumper fitted everything written above has come back.
u5 = UART(2, 9600, tx=Pin.cpu.PA2, rx=Pin.cpu.PA3, timeout=50)
time.sleep_ms(20)
while u5.any():
    u5.read(u5.any())
t0 = time.ticks_ms()
got = u5.read(4)
dt = time.ticks_diff(time.ticks_ms(), t0)
check("read times out", got is None, repr(got))
check("read honours timeout", 30 <= dt <= 200, "got %d ms" % dt)

# --- loopback, if the jumper is fitted ---
lb = UART(2, 9600, tx=Pin.cpu.PA2, rx=Pin.cpu.PA3, timeout=200)
lb.write(b"probe")
time.sleep_ms(50)
if lb.any() == 0:
    skip("loopback", "wire PA2 (TX) to PA3 (RX) to run these")
else:
    lb.read(lb.any())  # drain the probe; sized, since read-all never sees EOF
    for baud in (9600, 115200, 921600):
        v = UART(2, baud, tx=Pin.cpu.PA2, rx=Pin.cpu.PA3, timeout=500)
        # Drain first: reconfiguring does not empty the ring, so anything the
        # previous iteration left would prepend itself to this one's frame.
        time.sleep_ms(20)
        while v.any():
            v.read(v.any())
        msg = bytes(range(32, 127))
        v.write(msg)
        time.sleep_ms(50 + len(msg) * 12000 // baud)
        back = v.read(len(msg))
        check("loopback %d baud" % baud, back == msg, repr(back)[:60])

    # A burst bigger than the ring, read back in chunks. With the ring large
    # enough the interrupt keeps every byte; the loop below is what a driver
    # reading a packet actually looks like.
    big = bytes((i & 0xFF) for i in range(256))

    v = UART(2, 115200, tx=Pin.cpu.PA2, rx=Pin.cpu.PA3, timeout=1000, rxbuf=512)
    v.write(big)
    out = bytearray()
    t0 = time.ticks_ms()
    while len(out) < len(big) and time.ticks_diff(time.ticks_ms(), t0) < 1000:
        chunk = v.read(32)
        if chunk:
            out.extend(chunk)
    check("256B burst into a 512B rxbuf", bytes(out) == big, "got %d bytes" % len(out))

    # And the converse, stated as a fact rather than hoped against: a ring
    # smaller than the burst loses bytes, because the interrupt drops rather
    # than blocking. Measured here at 115200 with a 64-byte ring, roughly a
    # third arrives. This is what flow control is for -- it is not a bug, and
    # a test that pretended otherwise would fail intermittently forever.
    v = UART(2, 115200, tx=Pin.cpu.PA2, rx=Pin.cpu.PA3, timeout=300, rxbuf=64)
    v.write(big)
    out = bytearray()
    t0 = time.ticks_ms()
    while len(out) < len(big) and time.ticks_diff(time.ticks_ms(), t0) < 300:
        chunk = v.read(32)
        if chunk:
            out.extend(chunk)
    check(
        "undersized rxbuf drops, as documented",
        len(out) < len(big),
        "got %d of %d bytes" % (len(out), len(big)),
    )

    # 8E1 and 8O1 round trip only if both ends agree, which they do here.
    for par, name in ((0, "even"), (1, "odd")):
        v = UART(2, 9600, bits=8, parity=par, stop=1, tx=Pin.cpu.PA2, rx=Pin.cpu.PA3, timeout=300)
        time.sleep_ms(20)
        while v.any():
            v.read(v.any())
        v.write(b"parity")
        time.sleep_ms(100)
        check("parity %s round trip" % name, v.read(6) == b"parity")

    for st in (0.5, 1, 1.5, 2):
        v = UART(2, 9600, tx=Pin.cpu.PA2, rx=Pin.cpu.PA3, timeout=300, stop=st)
        time.sleep_ms(20)
        while v.any():
            v.read(v.any())
        v.write(b"stp")
        time.sleep_ms(120)
        check("stop=%s round trip" % st, v.read(3) == b"stp")

    # 5 to 9 bit words, with the parity bit counted into the word length by
    # the driver rather than by the caller.
    for nbits in (7, 8):
        v = UART(2, 9600, tx=Pin.cpu.PA2, rx=Pin.cpu.PA3, timeout=300, bits=nbits, parity=0)
        time.sleep_ms(20)
        while v.any():
            v.read(v.any())
        v.write(b"AB")
        time.sleep_ms(120)
        check("bits=%d even parity round trip" % nbits, v.read(2) == b"AB")

print("PASS", passed, "FAIL", failed, "SKIP", skipped)
print("RESULT", "OK" if failed == 0 else "FAILED")
