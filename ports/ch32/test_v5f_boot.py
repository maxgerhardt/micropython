"""Verify MicroPython boots on the V5F and reports the 400 MHz core clock."""
import sys, os
sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "scripts"))
from serial_expect import ReplSession

with ReplSession() as s:
    s.interrupt()
    s.expect(r">>>", timeout=15.0)
    s.eval("import machine; machine.freq()", r"400000000")
    s.expect(r">>>", timeout=5.0)
print("V5F BOOT PASS")
