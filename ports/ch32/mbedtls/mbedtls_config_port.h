#ifndef MICROPY_INCLUDED_MBEDTLS_CONFIG_H
#define MICROPY_INCLUDED_MBEDTLS_CONFIG_H

/* Hardware AES on the ECDC block. mbedtls calls the mbedtls_aes_* API in
 * ports/ch32/mbedtls/aes_alt.c instead of compiling its own software AES, so
 * this both accelerates TLS record encryption and drops the software tables
 * and round code from the image. */
#define MBEDTLS_AES_ALT

// Speeds up the ECC handshake, which is where a TLS connection spends its time.
#define MBEDTLS_ECP_NIST_OPTIM

/* Certificate validity dates need a wall clock. The RTC is the only thing here
 * that has one, and it only counts from a power-on epoch unless something sets
 * it -- see ch32_mbedtls_time() for what that means for verification. */
#include <time.h>
time_t ch32_mbedtls_time(time_t *timer);
#define MBEDTLS_PLATFORM_TIME_MACRO ch32_mbedtls_time

/* Separate from the wall clock above: mbedtls uses this one only for measuring
 * elapsed intervals, so it wants a monotonic millisecond counter rather than a
 * date, and SysTick is exactly that. */
#define MBEDTLS_PLATFORM_MS_TIME_ALT

// Set MicroPython-specific options.
#define MICROPY_MBEDTLS_CONFIG_BARE_METAL (1)

// Include common mbedtls configuration.
#include "extmod/mbedtls/mbedtls_config_common.h"

/******************************************************************************/
/* Trim to fit.
 *
 * This is not tuning for its own sake. The full configuration builds a 489 KB
 * image, and the usable code flash on this part is 480 KB unless DBMODE is
 * turned on -- the board came up completely dead, no boot banner, while
 * OpenOCD still reported "Verified OK". See ports/ch32/README.md.
 *
 * The undefs come after the common header rather than before it because that
 * header defines these unconditionally; this file is what mbedtls includes, so
 * removing them here still happens before anything reads them.
 *
 * What is kept: TLS 1.2, ECDHE-ECDSA, SHA-1/256/384, AES-CBC and AES-GCM, and
 * full X.509 chain verification.
 */

/* TLS 1.3 is a large chunk of code and its own key-schedule machinery. */
#undef MBEDTLS_SSL_PROTO_TLS1_3
#undef MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_ENABLED
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL_ENABLED

// No datagram TLS: this port has no use for it and it drags in its own timers.
#undef MBEDTLS_SSL_PROTO_DTLS
#undef MBEDTLS_SSL_DTLS_ANTI_REPLAY
#undef MBEDTLS_SSL_DTLS_HELLO_VERIFY
#undef MBEDTLS_SSL_DTLS_SRTP
#undef MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE
#undef MBEDTLS_SSL_DTLS_CONNECTION_ID
#undef MBEDTLS_TIMING_C

/* RSA is IN, up to 4096-bit keys.
 *
 * It was cut when the image had to fit in the 448 KB OpenOCD would program;
 * wlink writes the full 960 KB, so that constraint is gone. Keeping it out
 * would have been a poor trade anyway -- most public HTTPS servers still
 * present RSA certificates, so an ECDSA-only build fails the handshake
 * against them rather than negotiating something else.
 *
 * MBEDTLS_MPI_MAX_SIZE defaults to 1024 bytes, which covers 8192-bit moduli,
 * so 4096 needs no change here. Finite-field DH stays out: every modern
 * server offers ECDHE, and DHM is pure code size for no gain. */
#undef MBEDTLS_DHM_C
#undef MBEDTLS_KEY_EXCHANGE_DHE_RSA_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_DHE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_ECDHE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_RSA_PSK_ENABLED

/* Only the two curves anything real uses. Each dropped curve removes its
 * constants and its group arithmetic. */
#undef MBEDTLS_ECP_DP_SECP192R1_ENABLED
#undef MBEDTLS_ECP_DP_SECP224R1_ENABLED
#undef MBEDTLS_ECP_DP_SECP521R1_ENABLED
#undef MBEDTLS_ECP_DP_SECP192K1_ENABLED
#undef MBEDTLS_ECP_DP_SECP224K1_ENABLED
#undef MBEDTLS_ECP_DP_SECP256K1_ENABLED
#undef MBEDTLS_ECP_DP_BP256R1_ENABLED
#undef MBEDTLS_ECP_DP_BP384R1_ENABLED
#undef MBEDTLS_ECP_DP_BP512R1_ENABLED
#undef MBEDTLS_ECP_DP_CURVE448_ENABLED

// Ciphers other than AES, and modes other than CBC/GCM.
#undef MBEDTLS_ARIA_C
#undef MBEDTLS_CAMELLIA_C
#undef MBEDTLS_DES_C
#undef MBEDTLS_CHACHA20_C
#undef MBEDTLS_CHACHAPOLY_C
#undef MBEDTLS_POLY1305_C
#undef MBEDTLS_CIPHER_MODE_CFB
#undef MBEDTLS_CIPHER_MODE_OFB
#undef MBEDTLS_CIPHER_MODE_XTS
#undef MBEDTLS_NIST_KW_C
#undef MBEDTLS_CCM_C

// Hashes beyond SHA-1/224/256/384/512. MD5 stays: X.509 parsing still wants it.
#undef MBEDTLS_RIPEMD160_C
#undef MBEDTLS_SHA3_C

// Odds and ends this port has no path to.
#undef MBEDTLS_SSL_SESSION_TICKETS
#undef MBEDTLS_SSL_CONTEXT_SERIALIZATION
#undef MBEDTLS_SSL_RENEGOTIATION
#undef MBEDTLS_SSL_ALPN
#undef MBEDTLS_X509_CSR_PARSE_C
#undef MBEDTLS_X509_CREATE_C
#undef MBEDTLS_X509_CSR_WRITE_C
#undef MBEDTLS_X509_CRT_WRITE_C
#undef MBEDTLS_PEM_WRITE_C
#undef MBEDTLS_PKCS5_C
#undef MBEDTLS_PKCS12_C
#undef MBEDTLS_ARC4_C
#undef MBEDTLS_BLOWFISH_C

#endif /* MICROPY_INCLUDED_MBEDTLS_CONFIG_H */
