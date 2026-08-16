# The fft module, checked against signals whose spectrum is known beforehand.
#
# Needs no hardware. The point of testing an FFT this way is that a wrong one
# still produces a plausible-looking picture: a swapped twiddle sign, an
# off-by-one in the bit reversal or a mis-scaled stage all give a spectrum
# that moves with the music and is simply not the spectrum. Every check below
# names the bin the energy has to land in, and how big it has to be.
import math
import time
from array import array

import fft

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


def tone(n, bin_, amp, chans=1, chan=0, offset_bins=0.0):
    """n frames of a sine at the given bin, interleaved into chans channels."""
    buf = array("h", bytearray(2 * n * chans))
    k = (bin_ + offset_bins) * 2 * math.pi / n
    for i in range(n):
        buf[i * chans + chan] = int(amp * math.sin(k * i))
    return buf


def peak(mags, lo=0, hi=None):
    """(index, value) of the largest magnitude in a range."""
    hi = len(mags) if hi is None else hi
    best = lo
    for i in range(lo, hi):
        if mags[i] > mags[best]:
            best = i
    return best, mags[best]


print("fft")

# --- construction ---
raises("size must be a power of two", ValueError, lambda: fft.FFT(500))
raises("size has a lower bound", ValueError, lambda: fft.FFT(8))
raises("size has an upper bound", ValueError, lambda: fft.FFT(8192))

f = fft.FFT(256, window=False)
check("size() is what was asked for", f.size() == 256)
check("bins() is half of it", f.bins() == 128)

mags = array("I", bytearray(4 * f.bins()))

# --- DC ---
# Windowing is off for the analytic checks below: a Hann window is the right
# thing for looking at music, and exactly the wrong thing for asserting where
# a bin's energy lands, because it deliberately spreads each tone over three.
dc = array("h", bytearray(2 * 256))
for i in range(256):
    dc[i] = 5000
n = f.magnitude(dc, mags)
check("magnitude() returns the bin count", n == 128)
exact = 5000 * 256
check(
    "DC lands in bin 0, to within a rounding error (%d vs %d)" % (mags[0], exact),
    abs(mags[0] - exact) < exact // 1000,
)
check("and nowhere else", peak(mags, 1)[1] < 5000)

# --- a single tone on a bin centre ---
# A sine of amplitude A on a bin centre has magnitude A*N/2 there. Anything
# else -- half that, twice that, or the right size in the wrong bin -- means
# the transform is wrong however good the picture looks.
f.magnitude(tone(256, 10, 10000), mags)
where, height = peak(mags)
expect = 10000 * 256 // 2
check("a tone at bin 10 peaks at bin 10 (got %d)" % where, where == 10)
check(
    "and at the right height (%d, expected %d)" % (height, expect),
    abs(height - expect) < expect // 100,
)
check("with a clean floor either side", mags[7] < expect // 100 and mags[13] < expect // 100)

# Frequency really is bin * rate / size: bin 64 of a 256-point transform is a
# quarter of the sample rate, which is a sample pattern repeating every 4.
f.magnitude(tone(256, 64, 10000), mags)
check("a tone at bin 64 peaks at bin 64", peak(mags)[0] == 64)

# --- two tones at once ---
mixed = tone(256, 8, 8000)
high = tone(256, 40, 4000)
for i in range(256):
    mixed[i] += high[i]
f.magnitude(mixed, mags)
lo_i, lo_v = peak(mags, 0, 24)
hi_i, hi_v = peak(mags, 24)
check("two tones give two peaks, at 8 and 40", (lo_i, hi_i) == (8, 40))
check("in the right proportion (2:1)", abs(lo_v - 2 * hi_v) < lo_v // 20)

# --- stride and offset pick a channel ---
# The reason this exists: mp3.Decoder writes interleaved stereo and
# AudioOut.write() reads it, so a visualiser must be able to analyse one
# channel of that buffer without copying it first.
stereo = tone(256, 12, 9000, chans=2, chan=0)
right = tone(256, 33, 9000, chans=2, chan=1)
for i in range(512):
    stereo[i] += right[i]
f.magnitude(stereo, mags, stride=2, offset=0)
check("stride=2 offset=0 sees the left channel only", peak(mags)[0] == 12)
f.magnitude(stereo, mags, stride=2, offset=1)
check("stride=2 offset=1 sees the right channel only", peak(mags)[0] == 33)

# --- bounds ---
short = array("h", bytearray(2 * 255))
raises("a short source is refused", ValueError, lambda: f.magnitude(short, mags))
raises(
    "so is one that stride runs off the end of",
    ValueError,
    lambda: f.magnitude(tone(256, 4, 100), mags, stride=2),
)
small = array("I", bytearray(4 * 64))
raises(
    "and a destination too small for the bins",
    ValueError,
    lambda: f.magnitude(tone(256, 4, 100), small),
)
raises("stride must be positive", ValueError, lambda: f.magnitude(dc, mags, stride=0))

# --- the window earns its place ---
# A tone halfway between two bins is the worst case for leakage. Unwindowed it
# smears across the whole spectrum; Hann confines it to its neighbours. This is
# the difference between a display with a moving noise floor and one without.
between = tone(256, 10, 10000, offset_bins=0.5)
f.magnitude(between, mags)  # f has window=False
bare = mags[40]
w = fft.FFT(256)  # the same size, windowed
w.magnitude(between, mags)
windowed = mags[40]
check(
    "Hann cuts far-off leakage hard (%d -> %d)" % (bare, windowed),
    windowed * 10 < bare,
)
w.magnitude(tone(256, 10, 10000), mags)
check("and still puts a bin-centre tone in its own bin", peak(mags)[0] == 10)

# --- cost ---
# What the C module is for. A 512-point transform per displayed frame has to
# disappear next to decoding MP3 in real time, or the audio pays for the graph.
big = fft.FFT(512)
big_mags = array("I", bytearray(4 * big.bins()))
src = tone(1024, 20, 12000, chans=2)
t0 = time.ticks_us()
for _ in range(50):
    big.magnitude(src, big_mags, stride=2)
us = time.ticks_diff(time.ticks_us(), t0) // 50
print("  512-point transform of interleaved stereo: %d us" % us)
check("a 512-point transform costs under 5 ms", us < 5000)

print("%u passed, %u failed" % (passed, failed))
