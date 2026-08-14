# machine.TouchPad on the TKEY peripheral.
#
# TKEY is the ADC with two extra bits set: a measurement charges the pad for a
# fixed time, discharges it through the converter and reports how far it got,
# so more capacitance means a smaller number -- the same direction as the
# ESP32's touch peripheral.
#
# A finger is the obvious way to add capacitance and this cannot press one, so
# the check here uses whatever the board already provides: two pins shorted
# together load each other and read far lower than an isolated pin. On this
# board PB0 and PB1 are strapped, which reads about 1840 against about 4086 for
# a pin on its own. Without that strap the section skips.
#
# To try it with a finger, put a wire or a scrap of foil on PC4 and watch
# read() fall as you touch it.
#
# Run on the target: python scripts/install_file.py ports/ch32/test_touchpad.py
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


def sample(name, n=8, **kw):
    t = machine.TouchPad(machine.Pin(name), **kw)
    return [t.read() for _ in range(n)]


check("machine.TouchPad exists", hasattr(machine, "TouchPad"))

t = machine.TouchPad(machine.Pin("PC4"))
print("repr:", t)
check("repr names the pin and channel", "PC4" in repr(t) and "channel=14" in repr(t))

# --- a reading is stable.
#
# Capacitance measurement is noisy by nature, so what matters is that repeated
# reads of an untouched pad sit on top of each other; an application sets its
# threshold relative to a baseline it takes itself.
vals = sample("PC4", 16)
spread = max(vals) - min(vals)
print("  PC4 x16: min %d max %d spread %d" % (min(vals), max(vals), spread))
check("reading is in range", 0 < min(vals) < 65536)
check("reading is stable", spread <= 40)

# --- every ADC-capable pin can be a pad
for name, channel in (("PA6", 6), ("PA7", 7), ("PC5", 15), ("PB0", 8)):
    v = sample(name, 4)
    print("  %-4s %s" % (name, v))
    check("%s reads" % name, all(x > 0 for x in v))

# --- more capacitance reads lower.
#
# This is the property the whole class rests on. Two shorted pins stand in for
# a finger: they are a bigger plate and read lower.
bare = sum(sample("PC4", 8)) // 8
strapped = sum(sample("PB0", 8)) // 8
print("  isolated PC4 %d, strapped PB0 %d" % (bare, strapped))
if strapped < bare * 3 // 4:
    check("a loaded pad reads lower", True)
    other = sum(sample("PB1", 8)) // 8
    check("both ends of the strap read low", other < bare * 3 // 4)
else:
    print("  SKIP  loaded-pad check -- PB0 and PB1 do not look strapped")

# --- charge and discharge times are adjustable.
#
# Only visible on a pad that is not saturating: an isolated pin has so little
# capacitance that it discharges before the counter has moved, and reads the
# same 4087 whatever it was charged with. The strapped pair is loaded enough to
# show the difference, so this rides on the same condition as the check above.
if strapped < bare * 3 // 4:
    short = sum(sample("PB0", 4, charge=0x20)) // 4
    long = sum(sample("PB0", 4, charge=0x9F)) // 4
    print("  PB0 charge 0x20 -> %d, charge 0x9F -> %d" % (short, long))
    check("a shorter charge gives a smaller reading", short < long)
else:
    print("  SKIP  charge time -- needs a pad that is not saturating")

try:
    machine.TouchPad(machine.Pin("PC4"), charge=0)
    check("zero charge rejected", False)
except ValueError:
    check("zero charge rejected", True)

# --- pins without an ADC channel
try:
    machine.TouchPad(machine.Pin("PE5"))
    check("a pin with no ADC channel is rejected", False)
except ValueError:
    check("a pin with no ADC channel is rejected", True)

# --- ADC and TouchPad share the converter.
#
# TKEY *is* ADC1 with two bits set, so the bits are set for the measurement and
# cleared afterwards. Without that, whichever was constructed second would read
# the other's configuration.
adc = machine.ADC(machine.Pin("PC4"))
pad = machine.TouchPad(machine.Pin("PC5"))
pairs = [(adc.read_u16(), pad.read()) for _ in range(4)]
print("  interleaved:", pairs)
check("ADC still reads after a touch measurement", all(0 < a <= 65535 for a, _ in pairs))
check("touch still reads after an ADC conversion", all(0 < p < 65536 for _, p in pairs))

print("%u passed, %u failed" % (passed, failed))
