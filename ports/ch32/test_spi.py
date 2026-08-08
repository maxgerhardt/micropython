"""Verify machine.SPI on SPI1 (SCK=PA5, MISO=PA6, MOSI=PA7, CS=PA4).

PA4-PA7 are SPI1's default pins and all four sit in the 3.3 V VDDIO domain --
measured, not assumed: driving PA5, PA6 and PA7 high and reading each back
through the ADC gives 3.299 V. Most of this chip's other SPI options are on
VIO18, which comes up around 1.3 V and needs level shifting.

Works with either of two devices, whichever answers:

  * an RFM95 / SX127x radio -- RegVersion reads 0x12, and the carrier-frequency
    registers are freely writable and read back exactly;
  * a BMP280 (0x58) or BME280 (0x60) -- chip-id register plus a writable
    ctrl_meas.

Both are chosen for the same reason: every layer of the transfer is checkable
rather than merely well-formed. A constant with a known value proves MISO
carries the device's answer rather than a stuck line; a writable register
proves MOSI; and a burst read that must agree with the same registers read one
at a time proves multi-byte transfers, which catches a loop that returns the
first byte n times or drops to zeros after it.

Note the two parts number their address bit oppositely -- the SX127x sets the
top bit to *write*, the BMP280 sets it to *read* -- so the accessors are
per-device rather than shared.

Skips cleanly when nothing is attached, so it is safe to run unattended.
"""

import sys, os, time

sys.path.insert(
    0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "scripts")
)
from serial_expect import DEFAULT_PORT  # noqa: F401  (documents the port convention)

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import pyboard

PORT = os.environ.get("MPY_PORT", "COM7")

DEVICE_TEST = """
import time
from machine import SPI, SoftSPI, Pin

fail = []

def check(name, got, want):
    if got != want:
        fail.append("%s: got %r want %r" % (name, got, want))

cs = Pin('PA4', Pin.OUT)
cs.value(1)
spi = SPI(1, baudrate=1000000, polarity=0, phase=0)

def xfer(first, n):
    cs.value(0)
    spi.write(bytes([first]))
    buf = spi.read(n) if n else b''
    cs.value(1)
    return buf

# --- which device is on the bus? -------------------------------------------
# The SX127x reads with the top address bit clear, the BMP280 with it set, so
# try both rather than assuming.
version = xfer(0x42, 1)[0]          # SX127x RegVersion
chip_id = xfer(0xD0 | 0x80, 1)[0]   # BMP280 chip id

if version == 0x12:
    device = 'RFM95'
elif chip_id in (0x58, 0x60):
    device = 'BME280' if chip_id == 0x60 else 'BMP280'
else:
    device = None

print("PROBE version=0x%02X chipid=0x%02X" % (version, chip_id))
print("DEVICE", device)
print("PRESENT", device is not None)

if device == 'RFM95':
    ident_reg, ident_val = 0x42, 0x12
    # RegFrfMsb/Mid/Lsb: the carrier frequency. Writable in every mode, reads
    # back exactly, and setting it while not transmitting does nothing.
    wr_regs = (0x06, 0x07, 0x08)
    # 0x40-0x4F is DIO mapping, version and PA/PLL configuration: all static,
    # and it contains RegVersion at 0x42, so the burst carries a known
    # constant rather than merely being self-consistent.
    #
    # Deliberately NOT 0x06-0x15, which was the first choice here. That range
    # includes RegRssiValue at 0x11, a live ambient-RSSI reading. It reads
    # 0x00 until the receiver has measured something and then jumps to a real
    # value, which made the burst and the single reads disagree on exactly one
    # byte on the first run after power-up and never again. A test whose
    # correctness depends on a radio having already heard something is not a
    # test of this driver.
    burst_at, burst_len = 0x40, 16
    max_hz = 10000000               # SX127x SPI maximum
    def rd(reg, n=1):
        return xfer(reg & 0x7F, n)
    def wr(reg, val):
        cs.value(0)
        spi.write(bytes([reg | 0x80, val]))
        cs.value(1)
elif device is not None:
    ident_reg, ident_val = 0xD0, chip_id
    wr_regs = (0xF4,)
    burst_at, burst_len = 0x88, 24
    max_hz = 10000000               # BMP280 SPI maximum
    def rd(reg, n=1):
        return xfer(reg | 0x80, n)
    def wr(reg, val):
        cs.value(0)
        spi.write(bytes([reg & 0x7F, val]))
        cs.value(1)

if device is not None:
    # --- MISO carries the device's answer ---------------------------------
    check("identity register", rd(ident_reg)[0], ident_val)

    # --- MOSI works: registers that are writable and read back ------------
    # Reading back a value we chose is the only thing that proves the write
    # reached the device; no amount of reading alone can show it.
    saved = [rd(r)[0] for r in wr_regs]
    for val in (0x55, 0xAA, 0x00, 0xFF):
        for r in wr_regs:
            wr(r, val)
        for r in wr_regs:
            check("reg 0x%02X readback 0x%02X" % (r, val), rd(r)[0], val)
    for r, v in zip(wr_regs, saved):
        wr(r, v)
    for r, v in zip(wr_regs, saved):
        check("reg 0x%02X restored" % r, rd(r)[0], v)

    # --- multi-byte bursts -------------------------------------------------
    burst = rd(burst_at, burst_len)
    check("burst length", len(burst), burst_len)
    check("burst is not constant", len(set(burst)) > 1, True)
    singles = bytes(rd(burst_at + i)[0] for i in range(burst_len))
    check("burst matches single reads", singles, burst)
    if device == 'RFM95':
        # RegVersion sits inside the burst, so the burst is anchored to a
        # known value and not just to itself.
        check("burst contains the identity byte", burst[ident_reg - burst_at], ident_val)

    # --- baudrate quantisation is reported honestly ------------------------
    # SCK is HCLK/2**n, so an arbitrary request cannot be met exactly. The
    # driver must round *down* -- never overclock a device that documented a
    # maximum -- and say what it actually did.
    for req, want in ((1000000, 781250), (4000000, 3125000), (12500000, 12500000)):
        bus = SPI(1, baudrate=req)
        got = int(str(bus).split("baudrate=")[1].split(",")[0])
        check("baudrate %d rounds down" % req, got, want)
        check("baudrate %d not overclocked" % req, got <= req, True)

    # --- the device still answers at every rate up to its maximum ----------
    for hz in (390625, 1562500, 6250000, max_hz):
        spi = SPI(1, baudrate=hz, polarity=0, phase=0)
        check("identity at %d Hz" % hz, rd(ident_reg)[0], ident_val)
    spi = SPI(1, baudrate=1000000, polarity=0, phase=0)

    # --- mode 3 also works --------------------------------------------------
    # Both parts clock in mode 0 or mode 3, so this is a real check that CPOL
    # and CPHA reach the peripheral rather than being silently dropped.
    spi = SPI(1, baudrate=1000000, polarity=1, phase=1)
    check("mode 3 reads the same identity", rd(ident_reg)[0], ident_val)
    spi = SPI(1, baudrate=1000000, polarity=0, phase=0)

    # --- SoftSPI sees the same device ---------------------------------------
    # It bit-bangs the pins, so it takes them away from the SPI alternate
    # function; the hardware bus has to be re-created afterwards. This is the
    # positive control that caught a broken open-drain read on the I2C side.
    spi = SoftSPI(baudrate=200000, polarity=0, phase=0,
                  sck=Pin('PA5'), mosi=Pin('PA7'), miso=Pin('PA6'))
    check("SoftSPI reads the same identity", rd(ident_reg)[0], ident_val)
    spi = SPI(1, baudrate=1000000)
    check("hardware SPI works again", rd(ident_reg)[0], ident_val)

# --- argument checking, which needs no device at all ------------------------
try:
    SPI(9)
    fail.append("SPI(9) should have raised")
except ValueError:
    pass

try:
    SPI(1, sck=Pin('PB0'))          # PB0 is not an SPI pin on any bus
    fail.append("SPI(1, sck=PB0) should have raised")
except ValueError:
    pass

try:
    SPI(1, bits=16)
    fail.append("SPI(1, bits=16) should have raised")
except ValueError:
    pass

# Alternative pins the mux really can reach must be accepted.
alt = SPI(1, sck=Pin('PB3'), miso=Pin('PB4'), mosi=Pin('PB5'))
check("alternative pins accepted", 'sck=PB3' in str(alt), True)

print("FAILURES:", fail)
"""


def open_board():
    # The CDC port vanishes across a reset or a reflash and takes a second or
    # two to come back, so opening it immediately after one is a race.
    last = None
    for _ in range(20):
        try:
            return pyboard.Pyboard(PORT, 115200)
        except Exception as exc:
            last = exc
            time.sleep(1.0)
    raise SystemExit("could not open %s: %s" % (PORT, last))


def main():
    pyb = open_board()
    pyb.enter_raw_repl()
    try:
        out = pyb.exec_(DEVICE_TEST).decode()
    finally:
        pyb.exit_raw_repl()
        pyb.close()
    sys.stdout.write(out)
    if "FAILURES: []" not in out:
        raise SystemExit("SPI FAIL")
    if "PRESENT True" not in out:
        print("SPI SKIP (no RFM95 or BMP280 on PA5/PA6/PA7 with CS on PA4)")
        return
    print("SPI PASS")


if __name__ == "__main__":
    main()
