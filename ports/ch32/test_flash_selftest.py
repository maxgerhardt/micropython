"""Prove erase/program works in the upper flash, above OpenOCD's assumed 512 KB."""
import sys, os
sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "scripts"))
from serial_expect import expect_boot

expect_boot([
    r"FLASHTEST capacity=960K",
    r"FLASHTEST erase 0x080ee000 ok",
    r"FLASHTEST blank ok",
    r"FLASHTEST write ok",
    r"FLASHTEST verify ok",
], timeout=20.0)
print("FLASH SELFTEST PASS")
