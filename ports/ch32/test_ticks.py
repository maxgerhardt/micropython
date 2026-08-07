"""Verify SysTick-derived timing is accurate to within 5%."""
import re
import sys, os
sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "scripts"))
from serial_expect import expect_boot

text = expect_boot([r"ticks_ms delta = (\d+)", r"ticks_us delta = (\d+)"], timeout=16.0)

ms = int(re.search(r"ticks_ms delta = (\d+)", text).group(1))
us = int(re.search(r"ticks_us delta = (\d+)", text).group(1))

assert 950 <= ms <= 1050, "1s delay measured as {} ms".format(ms)
assert 950_000 <= us <= 1_050_000, "1s delay measured as {} us".format(us)
print("TICKS PASS ({} ms / {} us)".format(ms, us))
