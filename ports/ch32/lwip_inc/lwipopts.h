#ifndef MICROPY_INCLUDED_CH32_LWIP_LWIPOPTS_H
#define MICROPY_INCLUDED_CH32_LWIP_LWIPOPTS_H

#define LWIP_NETIF_EXT_STATUS_CALLBACK  1

#define LWIP_IPV6                       0

/* Seed and per-connection randomness come from the hardware TRNG rather than
 * lwip's default rand(). TCP initial sequence numbers and DHCP transaction IDs
 * are the callers; a predictable stream there is a real weakness, and this
 * port already conditions the TRNG properly -- see rng.c. */
#define LWIP_RAND() ch32_rng_u32()

/* Larger than the common defaults, which target parts with far less RAM.
 *
 * The common header offers three sizes; this is its largest (about 45 KB), and
 * it is the right one here: this is a 100 Mbit interface and the .bss these
 * arrays land in is DTCM, of which the board has 256 KB against a ~30 KB
 * firmware working set. A full 1460-byte MSS also avoids fragmenting every
 * outbound segment, which the 800-byte default would do.
 *
 * These must be defined BEFORE including the common header, which wraps its
 * own choice in #ifndef MEM_SIZE. */
#define MEM_SIZE                        (16000)
#define TCP_MSS                         (1460)
#define TCP_WND                         (8 * TCP_MSS)
#define TCP_SND_BUF                     (8 * TCP_MSS)
#define MEMP_NUM_TCP_SEG                (32)

/* One pool buffer holds a whole maximum-size frame, so the receive path never
 * has to chain: the driver hands lwip the frame with the 14-byte Ethernet
 * header still attached, and lwip's default sizes this from TCP_MSS plus the
 * IP/TCP headers without accounting for that.
 *
 * Set explicitly rather than left to the default, but note this did NOT fix
 * the bulk-inbound reset described in ports/ch32/README.md -- that reproduces
 * identically with chaining removed, so whatever it is, it is not this. */
#define PBUF_POOL_BUFSIZE               (1536)
#define PBUF_POOL_SIZE                  (16)

// Include common lwIP configuration (also mpconfig.h).
#include "extmod/lwip-include/lwipopts_common.h"

extern uint32_t ch32_rng_u32(void);

#endif // MICROPY_INCLUDED_CH32_LWIP_LWIPOPTS_H
