"""Verify buffered RX: a burst sent while the target is busy is not dropped."""
import sys, os, time
sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "scripts"))
from serial_expect import ReplSession, reset_target

with ReplSession() as s:
    s.ser.reset_input_buffer()
    reset_target()
    # Sync on the last boot marker rather than guessing: the target has a boot
    # delay and a 1 s timing self-check before the echo loop starts.
    s.expect(r"ticks_us delta", timeout=20.0)
    # The target echoes each buffered burst back as "rx:<bytes>". Send while it
    # is inside its 500 ms busy window, where polled RX would lose bytes.
    s.ser.write(b"ABCDEFGH")
    s.expect(r"rx:ABCDEFGH", timeout=12.0)
print("UART RX PASS")
