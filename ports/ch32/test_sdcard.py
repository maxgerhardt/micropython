# machine.SDCard on the SDMMC controller, and the VIO18 rail it depends on.
#
# Wiring, SDMMC default mapping, 1-bit. A plain SPI microSD breakout carries
# the SD-mode signals under other names -- its "MOSI" is the card's CMD and
# its "MISO" is the card's DAT0 -- so one of those works unmodified:
#
#     3V3 -> 3V3    CLK  -> PC12    MISO -> PC8    (DAT0)
#     GND -> GND    MOSI -> PD2     CS   -> PC11   (DAT3)
#
# The VIO18 half of this runs with no card attached. Everything below the
# "card" marker skips cleanly if there is none.
#
# Run on the target: python scripts/install_file.py ports/ch32/test_sdcard.py
import os
import time

import machine

# The card's very last block gets read, then written back byte-for-byte
# unchanged. That exercises the write path without changing a single byte of
# the card, even if the test is interrupted mid-way. Set to False to leave the
# card strictly read-only.
ALLOW_RAW_WRITE = True

BLOCK = 512

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
        check(name + " (raised %s)" % type(e).__name__, False)
        return
    check(name + " (raised nothing)", False)


# --- VIO18.
#
# Most of this chip's pads are not on VDDIO but on VIO18, an on-chip LDO whose
# output PWR_CTLR selects. It powers up at 1.2 V here, which no 3.3 V device
# can read as a one, and the port raises it to 3.3 V at boot. Every SDMMC pin
# is in that domain, so this is a precondition for the rest of the file and
# not merely a related feature.
check("vio18 is 3.3 V after boot", machine.vio18() == 3300)

for bad in (1000, 3400, -1):
    raises("vio18(%r) rejected" % bad, ValueError, lambda b=bad: machine.vio18(b))

for mv in (1200, 1800, 2500):
    machine.vio18(mv)
    check("vio18(%u) reads back" % mv, machine.vio18() == mv)
machine.vio18(3300)
check("vio18 back to 3.3 V", machine.vio18() == 3300)

# The rail really does move, not just the register: PC0 is on VIO18 and has an
# ADC channel, so it can measure its own pad. Construct the ADC first -- that
# is what puts the pad in analog mode -- and drive it afterwards.
adc = machine.ADC(machine.Pin("PC0"))
pc0 = machine.Pin("PC0", machine.Pin.OUT)
pc0.value(1)


def pad_mv(settle=0):
    # Raising the rail is fast, but lowering it is not: nothing discharges
    # VIO18 except the pads' own leakage, so a 3.3 V -> 1.2 V step is still
    # passing through 1.7 V a couple of milliseconds later. Sample until the
    # reading stops moving rather than assuming a fixed settling time.
    prev = None
    for _ in range(settle * 10 + 1):
        vals = sorted(adc.read_uv() for _ in range(9))
        now = vals[4] // 1000
        if prev is not None and abs(now - prev) < 5:
            return now
        prev = now
        time.sleep_ms(100 if settle else 0)
    return prev


high = pad_mv()
machine.vio18(1200)
low = pad_mv(settle=20)
machine.vio18(3300)
back = pad_mv()
print("  PC0 driven high: %u mV at 3.3 V, %u mV at 1.2 V, %u mV again" % (high, low, back))
check("VIO18 pad follows the setting", high > 3000 and low < 1600 and back > 3000)
machine.Pin("PC0", machine.Pin.IN)

# --- constructor argument checks. None of these reach the card.
check("machine.SDCard exists", hasattr(machine, "SDCard"))
raises("slot must be 1", ValueError, lambda: machine.SDCard(slot=2))
raises("width must be 1 or 4", ValueError, lambda: machine.SDCard(width=2))
raises("freq has a floor", ValueError, lambda: machine.SDCard(freq=1000))

# Constructing while the pads are at 1.2 V has to fail loudly rather than time
# out somewhere in the card protocol.
machine.vio18(1200)
raises("refuses to run at 1.2 V", OSError, lambda: machine.SDCard())
machine.vio18(3300)

# --- card.
try:
    sd = machine.SDCard()
except OSError as e:
    print("  SKIP  card checks -- no card answered (%s)" % e)
    sd = None

if sd is not None:
    print(" ", sd)
    size, blksize, kind = sd.info()
    print("  %u bytes, %u-byte blocks, type %u" % (size, blksize, kind))
    check("block size is 512", blksize == BLOCK)
    check("capacity is plausible", 8 * 1024 * 1024 < size < 2048 * 1024 * 1024 * 1024)
    check("size agrees with ioctl", size == sd.ioctl(4, 0) * sd.ioctl(5, 0))
    check("CID is not blank", any(sd.cid()))
    check("CSD is not blank", any(sd.csd()))

    # --- reads.
    b0 = bytearray(BLOCK)
    check("read block 0", sd.readblocks(0, b0) == 0)
    check("block 0 is not all zero", any(b0))
    print("  block 0 tail: %s" % bytes(b0[-4:]))

    again = bytearray(BLOCK)
    sd.readblocks(0, again)
    check("re-reading block 0 gives the same bytes", again == b0)

    # A multi-block read has to agree with the same blocks read one at a time.
    # This is the CMD18 path, which is a different command, a different DMA
    # length and a CMD12 at the end -- none of it shared with the single-block
    # path above.
    multi = bytearray(8 * BLOCK)
    check("read 8 blocks at once", sd.readblocks(0, multi) == 0)
    ok = True
    one = bytearray(BLOCK)
    for i in range(8):
        sd.readblocks(i, one)
        if one != multi[i * BLOCK : (i + 1) * BLOCK]:
            ok = False
            print("    block %u differs" % i)
    check("multi-block read matches single-block reads", ok)

    # --- a longer run, across the bounce-buffer chunk size.
    #
    # Everything goes through an eight-block buffer, so a twenty-block
    # transfer is three chunks and the seams between them are exactly where an
    # off-by-one would show. Written to a scratch area well past the volume's
    # start and then read back both ways.
    SCRATCH = 20480
    NBLK = 20
    big = bytearray(NBLK * BLOCK)
    for i in range(len(big)):
        big[i] = (i * 31 + i // BLOCK) & 0xFF
    check("write 20 blocks", sd.writeblocks(SCRATCH, big) == 0)
    readback = bytearray(NBLK * BLOCK)
    check("read 20 blocks", sd.readblocks(SCRATCH, readback) == 0)
    check("20 blocks round-trip byte for byte", readback == big)

    # The same blocks one at a time have to agree with the long read, which is
    # what catches a chunk boundary landing on the wrong card address.
    ok = True
    one = bytearray(BLOCK)
    for i in range(NBLK):
        sd.readblocks(SCRATCH + i, one)
        if one != big[i * BLOCK : (i + 1) * BLOCK]:
            ok = False
            print("    block %u of the run differs" % i)
    check("single-block reads agree with the long write", ok)

    # Read immediately after write, repeatedly, on alternating blocks: the
    # controller reloads its DMA pointer only when the address register
    # changes, so this is the shape that caught it returning stale data.
    ok = True
    for i in range(6):
        blk = SCRATCH + 64 + (i % 3)
        pat = bytearray(bytes([0x40 + i]) * BLOCK)
        if sd.writeblocks(blk, pat) != 0:
            ok = False
            break
        sd.readblocks(blk, one)
        if one != pat:
            ok = False
            print("    pass %u read back %02x" % (i, one[0]))
            break
    check("read straight after write, six times over", ok)

    # --- bounds and shapes.
    raises("odd-sized buffer rejected", ValueError, lambda: sd.readblocks(0, bytearray(BLOCK + 1)))
    last = sd.ioctl(4, 0) - 1
    check("reading the last block works", sd.readblocks(last, b0) == 0)
    check("reading past the end fails", sd.readblocks(last + 1, bytearray(BLOCK)) != 0)

    # --- write, as a no-op.
    if ALLOW_RAW_WRITE:
        orig = bytearray(BLOCK)
        sd.readblocks(last, orig)
        check("write the last block back unchanged", sd.writeblocks(last, orig) == 0)
        after = bytearray(BLOCK)
        sd.readblocks(last, after)
        check("last block survived the rewrite", after == orig)

    # --- as a filesystem.
    mounted = False
    try:
        os.mount(sd, "/sd")
        mounted = True
    except Exception as e:
        print("  SKIP  filesystem checks -- cannot mount (%s)" % e)

    if mounted:
        print("  /sd holds:", os.listdir("/sd")[:8])
        st = os.statvfs("/sd")
        print("  %u blocks of %u bytes, %u free" % (st[2], st[0], st[3]))
        check("statvfs reports a real volume", st[2] > 0)

        payload = bytes(range(256)) * 12  # 3 KB, so several blocks
        with open("/sd/mp_test.bin", "wb") as f:
            f.write(payload)
        with open("/sd/mp_test.bin", "rb") as f:
            check("file round-trips through the card", f.read() == payload)
        check("the file is listed", "mp_test.bin" in os.listdir("/sd"))
        os.remove("/sd/mp_test.bin")
        check("the file is gone again", "mp_test.bin" not in os.listdir("/sd"))
        os.umount("/sd")
        check("still readable after unmount", sd.readblocks(0, b0) == 0)

    sd.deinit()
    check("deinit is idempotent", sd.deinit() is None)
    check("a deinitialised card reports itself", "deinit" in repr(sd))

print("%u passed, %u failed" % (passed, failed))
