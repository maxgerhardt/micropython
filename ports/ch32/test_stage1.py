"""Stage-1 verification: the MicroPython image boots and reports its heap."""
import sys, os
sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "scripts"))
from serial_expect import expect_boot

expect_boot([
    r"MicroPython on .*CH32H417",
    r"heap: \d+ bytes",
    r"mp_init ok",
], timeout=14.0)
print("STAGE1 PASS")
