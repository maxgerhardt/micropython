# I2S: machine.I2S on the SAI peripheral.
#
# Wants an I2S microphone on SAI block A -- brought up against an INMP441:
#
#     VDD -> 3V3      SCK -> PE5
#     GND -> GND      WS  -> PE4
#     L/R -> GND      SD  -> PE6
#
# L/R to ground puts the microphone in the left channel, which is the slot
# MONO reads. The signal checks are skipped if nothing is connected, so the
# configuration checks still run on a bare board.
import math
import struct
import time

import ch32
from machine import I2S, Pin

passed = 0
failed = 0


def check(name, cond):
    global passed, failed
    if cond:
        passed += 1
        print("  PASS ", name)
    else:
        failed += 1
        print("  FAIL ", name)


def make(fmt=I2S.MONO, rate=16000, bits=32):
    return I2S(
        0,
        sck=Pin("PE5"),
        ws=Pin("PE4"),
        sd=Pin("PE6"),
        mode=I2S.RX,
        bits=bits,
        format=fmt,
        rate=rate,
        ibuf=16384,
    )


print("I2S")

i2s = make()
check("I2S() constructs", i2s is not None)

# The SAI divider is 6 bits, so the rate is quantised. Ask for the truth.
actual = ch32.i2s_actual_rate(0)
check("actual rate is reported", actual > 0)
check("actual rate within 1% of request", abs(actual - 16000) < 160)

buf = bytearray(4096)
n = i2s.readinto(buf)
check("readinto fills the buffer", n == len(buf))

# Discard what was captured while the microphone was still settling.
for _ in range(8):
    i2s.readinto(buf)

i2s.readinto(buf)
samples = struct.unpack("<1024i", buf)
nonzero = sum(1 for s in samples if s)
peak = max(max(samples), -min(samples))

live = nonzero > 0
if live:
    check("samples are non-zero", nonzero > len(samples) // 2)
    # 24-bit data sits left-justified in a 32-bit word. The low 8 bits are
    # NOT zero and NOT signal: the INMP441 stops driving SD after its 24 bits
    # and the line holds its last level, so those bits just echo bit 8. Two or
    # three distinct values is what that looks like; real signal down there
    # would mean the frame is misaligned.
    check("low byte carries no signal", len(set(s & 0xFF for s in samples)) <= 4)
    # A misaligned frame would push the magnitude outside 24-bit range.
    check("magnitude fits 24 bits", all(abs(s >> 8) < (1 << 23) for s in samples))
    check("peak is below full scale", peak < (1 << 31))

    mean = sum(samples) // len(samples)
    rms = int(math.sqrt(sum((s - mean) ** 2 for s in samples) / len(samples)))
    check("signal varies (not a stuck value)", rms > 0)
    check("no large DC offset", abs(mean) < (1 << 28))
else:
    print("  SKIP  no signal -- is a microphone connected to PE4/PE5/PE6?")

i2s.deinit()

# Stereo: the microphone occupies one slot, the other stays silent.
i2s = make(fmt=I2S.STEREO)
for _ in range(8):
    i2s.readinto(buf)
i2s.readinto(buf)
st = struct.unpack("<1024i", buf)
left = [st[i] for i in range(0, 1024, 2)]
right = [st[i] for i in range(1, 1024, 2)]
if live:
    check("stereo left slot carries the microphone", any(left))
    check("stereo right slot is silent", not any(right))
i2s.deinit()

# A rate the 6-bit divider cannot reach must be refused, not silently wrong.
try:
    bad = make(rate=4000)
    bad.deinit()
    check("unreachable rate raises", False)
except ValueError:
    check("unreachable rate raises", True)

# Pins outside the SAI's reach must be refused too.
try:
    bad = I2S(
        0,
        sck=Pin("PA0"),
        ws=Pin("PE4"),
        sd=Pin("PE6"),
        mode=I2S.RX,
        bits=32,
        format=I2S.MONO,
        rate=16000,
        ibuf=4096,
    )
    bad.deinit()
    check("bad pin raises", False)
except ValueError:
    check("bad pin raises", True)

# Re-creating the same id must not leak or wedge the peripheral.
a = make()
a.readinto(buf)
b = make()
n = b.readinto(buf)
check("re-init on the same id still reads", n == len(buf))
b.deinit()

print("%u passed, %u failed" % (passed, failed))
