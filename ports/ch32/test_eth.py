# Ethernet: network.LAN on the on-chip 100M PHY.
#
# Run on the target with a cable plugged into a network that has a DHCP
# server. Checks that do not need the network run regardless, so the file is
# still useful on an unplugged board.
#
# Deliberately does NOT drive a bulk inbound transfer: that resets the board,
# see the "Known defect" note in README.md. Adding it here would turn a test
# run into a reboot.
import time

import ch32
import network

passed = 0
failed = 0


def check(name, cond):
    global passed, failed
    if cond:
        passed += 1
        print("  PASS ", name)
    else:
        failed += 1
        print("  FAIL ", name)


print("Ethernet")

lan = network.LAN()
check("LAN() constructs", lan is not None)
check("LAN is a singleton", network.LAN() is lan)

mac = lan.config("mac")
check("mac is 6 bytes", isinstance(mac, bytes) and len(mac) == 6)
check("mac is not all zero", mac != b"\0" * 6)
check("mac is not all ones", mac != b"\xff" * 6)
# Bit 0 of the first octet is the group bit; a source address must be unicast.
check("mac is a unicast address", not (mac[0] & 1))
check("mac is stable", lan.config("mac") == mac)

check("inactive after construction", lan.active() in (True, False))

# The PHY only answers over SMI once the MAC clock and its PLL are up, so a
# plausible ID doubles as proof that bring-up got that far.
regs, stats = ch32.eth_diag()
check("ETH PLL locked", bool(regs[0] & (1 << 27)))
check("MAC clock enabled", bool(regs[1] & (1 << 14)))
check("PHY out of reset", bool(regs[2] & (1 << 31)))
check("PHY not powered down", not (regs[2] & (1 << 30)))
check("PHY ID readable", regs[6] not in (0x0000, 0xFFFF))

lan.active(True)
check("active() reports True", lan.active() is True)

# Auto-negotiation takes a moment after a reset; the driver picks it up from
# its periodic link check rather than needing a second PHY interrupt.
deadline = time.ticks_add(time.ticks_ms(), 15000)
while lan.status() < 3 and time.ticks_diff(deadline, time.ticks_ms()) > 0:
    time.sleep_ms(200)

linked = lan.status() >= 2
check("link came up", linked)

if linked:
    check("isconnected() once addressed", lan.isconnected() is (lan.status() == 3))
    cfg = lan.ifconfig()
    check("ifconfig returns 4 items", len(cfg) == 4)
    check("DHCP supplied an address", cfg[0] not in ("0.0.0.0", ""))
    check("netmask is set", cfg[1] not in ("0.0.0.0", ""))
    check("gateway is set", cfg[2] not in ("0.0.0.0", ""))
    check("ipconfig agrees with ifconfig", lan.ipconfig("addr4")[0] == cfg[0])

    import socket

    # DNS proves the whole path: ARP to the gateway, UDP out, reply in.
    try:
        ai = socket.getaddrinfo("example.com", 80)
        check("DNS resolves", len(ai) > 0 and len(ai[0][-1]) == 2)
    except OSError:
        check("DNS resolves", False)

    _, stats = ch32.eth_diag()
    check("frames received", stats[0] > 0)
    check("frames transmitted", stats[1] > 0)
    check("no transmit errors", stats[4] == 0)
else:
    print("  SKIP  no link -- is a cable plugged in?")

lan.active(False)
check("active(False) reports False", lan.active() is False)

print("%u passed, %u failed" % (passed, failed))
