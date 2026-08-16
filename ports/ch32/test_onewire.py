# 1-Wire, bit-banged on a GPIO.
#
# There is no 1-Wire peripheral on this part -- SWI is a smartcard interface
# and speaks nothing like it -- and none is needed: extmod/modonewire.c does
# the whole protocol on one open-drain pin inside the same critical section
# dht_readinto() and machine.bitstream() use.
#
# Runs in two halves. The first needs no sensor: it checks the CRC, that the
# module and its two Python drivers are frozen in, and -- the part that
# actually exercises the timing -- that a reset pulse reports *no* presence on
# an idle bus and a phantom one on a floating bus. The second half wants a
# real DS18B20 on PB5 and skips cleanly without one; it is what shows that the
# bus can be talked to rather than merely driven.
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
import time

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


def raises(name, exc, fn):
    try:
        fn()
    except exc:
        check(name, True)
        return
    except Exception as e:
        check("%s (got %s)" % (name, type(e).__name__), False)
        return
    check("%s (nothing raised)" % name, False)


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

# --- with a real DS18B20 on the bus.
#
# Everything above works on an empty bus, which proves the timing but not that
# anything can be talked to. These need a DS18B20 on DEVICE_BUS and its 4.7k
# to 3V3 -- the usual 3-pin module carries the resistor already. They skip
# cleanly with nothing attached.
DEVICE_BUS = "PB5"

# What a 12-bit conversion is allowed to take. Every reading here sleeps it
# out rather than asking the device, for the reason set out at busy_poll().
CONVERT_MS = 750

# Config register values and what each resolution quantises to, in 1/16 C.
# Only meaningful if the device honours the register at all -- see below.
RESOLUTIONS = (
    (9, 0x1F, 8),
    (12, 0x7F, 1),
)


def busy_poll(ow, limit_ms=1000):
    """Milliseconds until the device reports its conversion done, or -1.

    A DS18B20 is specified to answer read slots with 0 while it is converting
    and 1 once the result is ready, which would let a caller wait exactly as
    long as it must. Returns 0 when the very first slot already reads done --
    which is not an instant conversion but a device that does not implement
    the signalling, and is what the part on this board does.
    """
    t0 = time.ticks_ms()
    while not ow.readbit():
        if time.ticks_diff(time.ticks_ms(), t0) > limit_ms:
            return -1
    return time.ticks_diff(time.ticks_ms(), t0)


def retry(fn, tries=3):
    """Call fn, retrying a transaction that came back unanswered.

    1-Wire carries no error correction: a transaction that is disturbed is
    simply not answered, every byte reads back as 0xff from the pull-up, and
    the CRC refuses it. Retrying is what a caller is expected to do, and on
    this board roughly one transaction in a hundred needs it -- see the rate
    measured at the end of this file. A test that did not retry would fail
    now and then for reasons that have nothing to do with what it checks.
    """
    for attempt in range(tries):
        try:
            return fn()
        except Exception:
            if attempt == tries - 1:
                raise


def read_after_convert(ds, rom):
    """One fresh reading, sleeping the datasheet time rather than asking.

    Reading sooner does not fail or raise: it returns the *previous*
    conversion's answer, which looks entirely reasonable and is wrong. That
    is worth a helper of its own so no check here can drift into doing it.
    """
    ds.convert_temp()
    time.sleep_ms(CONVERT_MS)
    return retry(lambda: ds.read_temp(rom))


def scratch(ds, rom):
    """A copy of the scratchpad, retried past an unanswered transaction."""
    return bytes(retry(lambda: ds.read_scratch(rom)))


def set_resolution(ds, rom, config):
    """Write the config byte, preserving the alarm bytes beside it."""
    sp = scratch(ds, rom)
    ds.write_scratch(rom, bytes((sp[2], sp[3], config)))


dev_idle = machine.Pin(DEVICE_BUS, machine.Pin.IN).value()
dev_ow = onewire.OneWire(machine.Pin(DEVICE_BUS))
dev_present = dev_ow.reset()
if not dev_idle or not dev_present:
    # Both halves are reported because they mean different things: a bus that
    # idles low has no pull-up and will fake a presence pulse, while one that
    # idles high with no answer really is empty.
    print(
        "  SKIP  DS18B20 checks -- %s idles %s, reset %s a presence pulse"
        % (
            DEVICE_BUS,
            "high" if dev_idle else "low (no pull-up!)",
            "saw" if dev_present else "saw no",
        )
    )
else:
    check("a device answers the reset on " + DEVICE_BUS, dev_ow.reset())

    roms = dev_ow.scan()
    print("  scan found:", [bytes(r).hex() for r in roms])
    check("scan finds exactly one device", len(roms) == 1)
    rom = roms[0]
    check("its ROM passes its own CRC", dev_ow.crc8(rom) == 0)
    check("its family code is 0x28 (DS18B20)", rom[0] == 0x28)

    ds = ds18x20.DS18X20(dev_ow)
    check("the ds18x20 driver recognises it", [bytes(r) for r in ds.scan()] == [bytes(rom)])

    # Addressing. read_scratch() goes through MATCH_ROM, so a ROM that is one
    # bit wrong must reach nothing at all: every read slot then comes back
    # from the pull-up as 0xff and the CRC refuses it. Getting an answer here
    # would mean select_rom() is not actually selecting.
    wrong = bytearray(rom)
    wrong[1] ^= 0xFF
    raises("a wrong ROM addresses no device", Exception, lambda: ds.read_scratch(wrong))
    check("and the right one still works after that", dev_ow.crc8(scratch(ds, rom)) == 0)

    # Which supply the part is on. It decides whether the busy-poll further
    # down is even permitted -- the datasheet rules it out for parasite-
    # powered devices -- so this separates "may not" from "does not".
    dev_ow.reset(True)
    dev_ow.select_rom(rom)
    dev_ow.writebyte(0xB4)  # READ POWER SUPPLY
    powered = dev_ow.readbit()
    print("  power supply: %s" % ("external" if powered else "parasite"))

    # write_scratch, checked on the alarm bytes rather than the config byte
    # beside them: every DS18B20 stores TH and TL, and these values are
    # distinctive enough that nothing else could have produced them.
    original = scratch(ds, rom)
    ds.write_scratch(rom, bytes((0x50, 0x11, original[4])))
    written = scratch(ds, rom)
    check("write_scratch reaches the device", written[2] == 0x50 and written[3] == 0x11)
    ds.write_scratch(rom, original[2:5])
    check(
        "and the alarm bytes can be put back",
        scratch(ds, rom)[2:5] == original[2:5],
    )

    # Resolution, if this device implements it. The config register is the
    # part of the scratchpad a clone is most likely to ignore: several parts
    # sold as DS18B20 are permanently 12-bit and drop writes to it silently.
    # Since write_scratch was just shown to work on the bytes either side of
    # it, a config byte that will not stick is the device's doing and not the
    # driver's, so it is reported rather than failed.
    set_resolution(ds, rom, 0x1F)
    if scratch(ds, rom)[4] != 0x1F:
        print(
            "  config register ignored (stays 0x%02x) -- a fixed 12-bit part,"
            % scratch(ds, rom)[4]
        )
        print("  so the per-resolution checks do not apply to it")
        ds.write_scratch(rom, original[2:5])
    else:
        for bits, config, step in RESOLUTIONS:
            set_resolution(ds, rom, config)
            check(
                "%d-bit config byte reads back" % bits,
                scratch(ds, rom)[4] == config,
            )
            # Reduced resolution clears the low bits of the result, so a
            # 9-bit reading can only land on a multiple of half a degree.
            # Nothing about reading alone changes a value's granularity, so
            # this is what shows the write took effect rather than merely
            # being stored.
            t = read_after_convert(ds, rom)
            check(
                "%d-bit reading quantises to %d/16 C (%.4f)" % (bits, step, t),
                int(round(t * 16)) % step == 0,
            )
        set_resolution(ds, rom, 0x7F)

    t = read_after_convert(ds, rom)
    check("a reading is a plausible temperature (%.4f C)" % t, -20.0 < t < 70.0)
    # 85.0 exactly is what a DS18B20 powers up holding, so it is also what a
    # conversion that never ran looks like. Named apart from the range check
    # because it sits inside that range and means the opposite.
    check("a reading is not the 85.0 power-on default", abs(t - 85.0) > 0.01)
    check("its scratchpad passes CRC", dev_ow.crc8(scratch(ds, rom)) == 0)

    # Can a conversion be waited on rather than slept through? Reported and
    # not asserted, because it is a property of the part rather than of this
    # port. Worth reporting loudly: where it does not work, a reading taken
    # early does not fail or raise, it is quietly the previous answer.
    ds.convert_temp()
    ms = busy_poll(dev_ow)
    if ms <= 0:
        print("  busy-poll unsupported: the first read slot after a convert")
        print("  already reads done, so a reading must sleep %d ms" % CONVERT_MS)
    else:
        print("  conversion signalled complete after %d ms" % ms)
        check("a signalled conversion is not instant", ms > 10)

    # And what ignoring that costs, shown side by side.
    ds.convert_temp()
    hasty = ds.read_temp(rom)
    patient = read_after_convert(ds, rom)
    print("  read with no wait: %.4f C; read after %d ms: %.4f C" % (hasty, CONVERT_MS, patient))

    # Repeatability. One good reading can be luck; a run of them that agree
    # rules out a marginal bus that mostly works.
    temps = [read_after_convert(ds, rom) for _ in range(10)]
    spread = max(temps) - min(temps)
    print("  10 readings: %.4f to %.4f C" % (min(temps), max(temps)))
    check("ten readings agree to within 2 C (spread %.4f)" % spread, spread < 2.0)
    check("and none of them is 85.0", all(abs(t - 85.0) > 0.01 for t in temps))

    # How often a transaction goes unanswered, measured rather than assumed.
    #
    # Worth having as a number because 1-Wire fails silently rather than
    # loudly: the device stops responding, every byte reads back as 0xff from
    # the pull-up, and only the CRC catches it. Over 1000 transactions each,
    # a 10-byte MATCH_ROM read missed 1.9% and a 2-byte SKIP_ROM read 0.6%,
    # every one of the 25 recovered on the next attempt, and they were spread
    # evenly through both runs rather than arriving in bursts.
    #
    # Longer transactions failing more often is the shape of a bus disturbed
    # per slot rather than of a driver that has a command wrong -- and a
    # transaction that gets no answer at all, sometimes not even a presence
    # pulse, is a silent device rather than corrupted data. The bound below
    # is deliberately loose: it is here to catch a bus that has stopped
    # working, not to hold this part to a rate nobody has traced to a cause.
    trials = 200
    unanswered = 0
    probe = bytearray(9)
    for _ in range(trials):
        try:
            dev_ow.reset(True)  # raises when there is no presence pulse
            dev_ow.select_rom(rom)
            dev_ow.writebyte(0xBE)
            dev_ow.readinto(probe)
            if dev_ow.crc8(probe):
                unanswered += 1
        except Exception:
            unanswered += 1
    rate = 100.0 * unanswered / trials
    print("  %d of %d transactions unanswered (%.1f%%)" % (unanswered, trials, rate))
    check("the bus answers the large majority of the time (%.1f%%)" % rate, rate < 10.0)

machine.Pin(DEVICE_BUS, machine.Pin.IN)
print("%u passed, %u failed" % (passed, failed))
