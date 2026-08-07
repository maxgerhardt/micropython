"""Verify the interactive REPL responds to expressions."""
import sys, os
sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "scripts"))
from serial_expect import ReplSession

with ReplSession() as s:
    s.interrupt()
    s.expect(r">>>", timeout=8.0)
    s.eval("2+2", r"\n4\r?\n")
    s.eval("'ab'*3", r"ababab")
    s.eval("[x*x for x in range(5)]", r"\[0, 1, 4, 9, 16\]")
print("REPL PASS")
