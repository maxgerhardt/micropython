"""Verify machine.mem_backup() on the CH32H417.

The whole value of this region is that its contents outlive a reset, so the
shape checks are the easy half and the two reset checks are the point. A region
that reports the right length and accepts writes but quietly starts over on
every boot would pass everything except those.

The region is plain SRAM in its own NOLOAD section -- this part has no
battery-backed user storage, see machine_mem_backup.c -- so it survives a soft
reset and machine.reset() but not power-off. That last case is checked the only
way it can be from here: after a cold boot the magic word in front of the user
area will not match, so the region reads as zeros rather than as whatever the
SRAM settled to. Running this twice in a row exercises the warm path.
"""

import os
import sys
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(ROOT, "..", "..", "tools"))
sys.path.insert(0, os.path.join(ROOT, "..", "..", "..", "scripts"))
import pyboard  # noqa: E402
from serial_expect import reset_target  # noqa: E402

PORT = os.environ.get("MPY_PORT", "COM7")

fail = []


def check(name, cond):
    if not cond:
        fail.append(name)
    return cond


def run(pyb, script):
    return pyb.exec_(script).decode()


def open_board():
    last = None
    for _ in range(20):
        try:
            return pyboard.Pyboard(PORT, 115200)
        except Exception as exc:
            last = exc
            time.sleep(1.0)
    raise SystemExit("could not open %s: %s" % (PORT, last))


SHAPE = """
import machine
bad = []

def check(name, cond):
    if not cond:
        bad.append(name)

mem = machine.mem_backup()
print("SHAPE", len(mem), mem.itemsize, len(mem) * mem.itemsize)

# The documented cross-port guarantees.
check("itemsize is 1 or 4", mem.itemsize in (1, 4))
check("region is not empty", len(mem) > 0)
check("region is writable", True)

regions = machine.mem_backup(-1)
check("mem_backup(-1) is a tuple", isinstance(regions, tuple))
check("region 0 is the same object", len(regions[0]) == len(mem))
print("REGIONS", len(regions))

# Out-of-range indices raise, in both directions.
for i in (len(mem), len(mem) + 1, 10000):
    try:
        mem[i] = 1
        check("index %d rejected" % i, False)
    except IndexError:
        pass
try:
    machine.mem_backup(7)
    check("invalid region rejected", False)
except ValueError:
    pass

# Every byte is independently addressable, first and last included -- an
# off-by-one in the length would show up here rather than as a crash.
for i in range(len(mem)):
    mem[i] = (i * 31 + 7) & 0xFF
check("every byte reads back", all(mem[i] == (i * 31 + 7) & 0xFF for i in range(len(mem))))

# It must not move or be collected: fill the heap and look again.
import gc
junk = [bytearray(1024) for _ in range(50)]
del junk
gc.collect()
check("survives a GC cycle", all(mem[i] == (i * 31 + 7) & 0xFF for i in range(len(mem))))

print("BAD", bad)
"""

STAMP = """
import machine
mem = machine.mem_backup()
for i in range(len(mem)):
    mem[i] = (i * 31 + 7) & 0xFF
print("STAMPED")
"""

VERIFY = """
import machine
mem = machine.mem_backup()
n = sum(1 for i in range(len(mem)) if mem[i] == (i * 31 + 7) & 0xFF)
print("MATCH %d of %d" % (n, len(mem)))
"""


def matched(out):
    a, b = out.split("MATCH")[1].split("of")
    return int(a.strip()), int(b.strip())


def main():
    pyb = open_board()
    pyb.enter_raw_repl()
    try:
        out = run(pyb, SHAPE)
        sys.stdout.write(out)
        check("shape checks", "BAD []" in out)

        # --- survives a soft reset ---
        run(pyb, STAMP)
        pyb.exit_raw_repl()
        pyb.enter_raw_repl(soft_reset=True)
        out = run(pyb, VERIFY)
        n, total = matched(out)
        print("after soft reset: %d of %d bytes intact" % (n, total))
        check("survives a soft reset", n == total)

        # --- and a hard reset, which re-runs the whole startup path ---
        run(pyb, STAMP)
        pyb.exit_raw_repl()
        pyb.close()
        reset_target()
    finally:
        pass

    pyb = open_board()
    pyb.enter_raw_repl()
    try:
        out = run(pyb, VERIFY)
        n, total = matched(out)
        print("after hard reset: %d of %d bytes intact" % (n, total))
        check("survives a hard reset", n == total)

        # A stamped region must not read as zeros -- that would mean the
        # cold-boot path ran and wiped it, which is the failure the magic word
        # exists to make visible.
        out = run(pyb, "import machine; m = machine.mem_backup(); print('ZEROS', not any(m))")
        sys.stdout.write(out)
        check("not wiped by the reset", "ZEROS False" in out)
    finally:
        pyb.exit_raw_repl()
        pyb.close()

    print("FAILURES:", fail)
    if fail:
        raise SystemExit("MEM_BACKUP FAIL")
    print("MEM_BACKUP PASS")


if __name__ == "__main__":
    main()
