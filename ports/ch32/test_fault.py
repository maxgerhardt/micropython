"""Verify a bad access produces a diagnostic dump rather than a silent hang."""

import sys, os

sys.path.insert(
    0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "scripts")
)
from serial_expect import ReplSession

with ReplSession() as s:
    s.interrupt()
    s.expect(r">>>", timeout=8.0)
    # Must trap into the handler rather than wedging silently. Note that many
    # "unmapped" addresses (0x30000000, 0xE0000000) simply read as zero on this
    # bus; 0xFFFFFFFC is one that genuinely raises a load access fault.
    s.send("import machine; machine.mem32[0xFFFFFFFC]")
    s.expect(r"HARDFAULT", timeout=10.0)
    s.expect(r"mcause=0x0*5\b", timeout=5.0)  # 5 = load access fault
    s.expect(r"mepc=0x[0-9a-f]+", timeout=5.0)
    s.expect(r"mtval=0xfffffffc", timeout=5.0)  # handler reports the bad address
    # The handler resets the chip, so a usable REPL must come back.
    s.expect(r">>>", timeout=20.0)
print("FAULT PASS")
