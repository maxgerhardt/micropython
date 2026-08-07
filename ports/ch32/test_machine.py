"""Verify the machine module's basic surface."""
import sys, os
sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "scripts"))
from serial_expect import ReplSession

with ReplSession() as s:
    s.interrupt()
    s.expect(r">>>", timeout=8.0)
    s.eval("import machine; machine.freq()", r"100000000")
    # PB0 as an output: a driven pin must read back what was written.
    s.send("p = machine.Pin(16, machine.Pin.OUT)")
    s.expect(r">>>", timeout=5.0)
    s.eval("p.on(); p.value()", r"\n1\r?\n")
    s.eval("p.off(); p.value()", r"\n0\r?\n")
    s.eval("p", r"Pin\(B0\)")
    # time module rides on the same SysTick HAL.
    s.eval("import time; time.ticks_diff(time.ticks_ms(), 0) >= 0", r"True")
print("MACHINE PASS")
