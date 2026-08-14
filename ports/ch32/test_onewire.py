# 1-Wire, bit-banged on a GPIO.
#
# There is no 1-Wire peripheral on this part -- SWI is a smartcard interface
# and speaks nothing like it -- and none is needed: extmod/modonewire.c does
# the whole protocol on one open-drain pin inside the same critical section
# dht_readinto() and machine.bitstream() use.
#
# No DS18B20 is needed to run this. What it checks without one is the CRC, that
# the module and its two Python drivers are frozen in, and -- the part that
# actually exercises the timing -- that a reset pulse reports *no* presence on
# an idle bus and a phantom one on a floating bus.
#
# That second case is the trap worth knowing about: 1-Wire needs a pull-up, and
# this chip cannot supply one. Its GPIO has no pull in any output mode, so the
# pin.init(OPEN_DRAIN, PULL_UP) inside onewire.py does nothing here. A bus with
# no external resistor floats low, every reset() reports a device, and scan()
# then times out looking for one. Use the usual 4.7k to 3V3.
#
# The pull-up for this test comes from PB1, which is strapped to PB0 on this
# board: as an input with its pull-up enabled it is a weak pull-up on the bus
# and nothing drives against it. Without the strap the bus sections skip.
#
# Run on the target: python scripts/install_file.py ports/ch32/test_onewire.py
import machine
import onewire
import ds18x20
import _onewire

BUS = "PB0"
PULL = "PB1"

passed = 0
failed = 0


def check(name, cond):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print("FAIL", name)


check("onewire is frozen in", hasattr(onewire, "OneWire"))
check("ds18x20 is frozen in", hasattr(ds18x20, "DS18X20"))
print("_onewire:", sorted(n for n in dir(_onewire) if not n.startswith("__")))
for fn in ("reset", "readbit", "writebit", "readbyte", "writebyte", "crc8"):
    check("_onewire.%s exists" % fn, hasattr(_onewire, fn))

# --- CRC8.
#
# Pure computation, so it needs no bus at all. Dallas polynomial, checked
# against values computed independently on the host.
check("crc8 of an empty buffer", _onewire.crc8(bytearray()) == 0x00)
check("crc8 of one zero byte", _onewire.crc8(bytearray(b"\x00")) == 0x00)
check(
    "crc8 of 28 00 00 00 12 34 56",
    _onewire.crc8(bytearray(b"\x28\x00\x00\x00\x12\x34\x56")) == 0x0B,
)
check(
    "crc8 of 28 FF 64 1E 0C 6D 9E",
    _onewire.crc8(bytearray(b"\x28\xff\x64\x1e\x0c\x6d\x9e")) == 0x0D,
)
# A whole ROM including its own CRC byte comes out zero, which is how the
# driver validates one.
check(
    "a ROM including its CRC checks to zero",
    _onewire.crc8(bytearray(b"\x28\x00\x00\x00\x12\x34\x56\x0b")) == 0x00,
)

# --- is the strap there?
pull = machine.Pin(PULL, machine.Pin.IN, machine.Pin.PULL_UP)
strapped = machine.Pin(BUS, machine.Pin.IN).value() == 1

if not strapped:
    print("  SKIP  bus checks -- %s and %s do not look strapped" % (BUS, PULL))
else:
    # --- an idle bus with a pull-up has no device on it.
    #
    # reset() drives the line low for 480 us, releases it, and samples 70 us
    # later. With nothing to answer, the pull-up has taken the line high again
    # and the answer is False. Getting that right means the timing, the
    # open-drain writes and the read all work.
    ow = onewire.OneWire(machine.Pin(BUS))
    resets = [ow.reset() for _ in range(5)]
    print("  pulled up, no device: ", resets)
    check("no presence pulse on an idle bus", not any(resets))
    check("scan finds nothing", ds18x20.DS18X20(ow).scan() == [])

    # Reading a bit off an idle bus gives a one: the pull-up wins.
    bits = [ow.readbit() for _ in range(8)]
    print("  readbit on an idle bus:", bits)
    check("an idle bus reads as ones", all(bits))
    check("readbyte on an idle bus is 0xff", ow.readbyte() == 0xFF)

    # Writing must not raise or hang; nothing is listening.
    ow.writebyte(0xCC)
    ow.write(b"\x44")
    check("writing to an empty bus is harmless", True)

    # --- and without the pull-up.
    #
    # Documented rather than merely avoided: this is what a missing resistor
    # looks like, and it looks like a device rather than like nothing.
    pull.init(machine.Pin.IN)
    ow = onewire.OneWire(machine.Pin(BUS))
    floating = [ow.reset() for _ in range(5)]
    print("  floating, no pull-up:  ", floating)
    check("a floating bus reports a phantom device", all(floating))

machine.Pin(BUS, machine.Pin.IN)
machine.Pin(PULL, machine.Pin.IN)
print("%u passed, %u failed" % (passed, failed))
