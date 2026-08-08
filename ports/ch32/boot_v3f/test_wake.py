"""Stage-1 verification: the V3F stub wakes the V5F, which reports 400 MHz."""

import sys, os

sys.path.insert(
    0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "..", "scripts")
)
from serial_expect import expect_boot

expect_boot(
    [
        r"V3F: waking V5F",
        r"V5F: alive",
        r"V5F: core id = 1",  # NVIC_GetCurrentCoreID must report the V5F
        r"V5F: SystemCoreClock = 400000000",
        r"V5F: tick 3",
    ],
    timeout=20.0,
)
print("WAKE PASS")
