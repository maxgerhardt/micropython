# TLS and the ECDC AES accelerator.
#
# The AES checks are known-answer tests from FIPS-197 / SP 800-38A. They matter
# more than usual here: openwch/ch32h417 issue #10 reports the encrypt and
# decrypt mode values swapped in the vendor header, so which one this port uses
# is decided by these vectors rather than by the header. If a future SDK drop
# fixes the header, "AES-128 ECB encrypt" fails and the fix is to swap
# CH32_ECDC_ENCRYPT/DECRYPT in mbedtls/aes_alt.c.
import binascii

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


def h(s):
    return binascii.unhexlify(s)


print("mbedtls")

import cryptolib

# FIPS-197 C.1: the canonical AES-128 vector.
key128 = h("000102030405060708090a0b0c0d0e0f")
pt = h("00112233445566778899aabbccddeeff")
ct128 = h("69c4e0d86a7b0430d8cdb78070b4c55a")

got = cryptolib.aes(key128, 1).encrypt(pt)
check("AES-128 ECB encrypt", got == ct128)
check("AES-128 ECB decrypt", cryptolib.aes(key128, 1).decrypt(ct128) == pt)

# MicroPython's cryptolib takes 16- or 32-byte keys only, so AES-192 is not
# reachable from Python even though the hardware supports it.
key256 = h("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f")
ct256 = h("8ea2b7ca516745bfeafc49904b496089")
check("AES-256 ECB encrypt", cryptolib.aes(key256, 1).encrypt(pt) == ct256)
check("AES-256 ECB decrypt", cryptolib.aes(key256, 1).decrypt(ct256) == pt)

# A second block with the same object must not reuse the first block's result,
# which a cached-configuration bug would do.
a = cryptolib.aes(key128, 1)
b1 = a.encrypt(pt)
b2 = a.encrypt(h("ffeeddccbbaa99887766554433221100"))
check("successive blocks differ", b1 != b2)

# SP 800-38A F.2.1, AES-128 CBC.
cbc_key = h("2b7e151628aed2a6abf7158809cf4f3c")
cbc_iv = h("000102030405060708090a0b0c0d0e0f")
cbc_pt = h("6bc1bee22e409f96e93d7e117393172a")
cbc_ct = h("7649abac8119b246cee98e9b12e9197d")
check("AES-128 CBC encrypt", cryptolib.aes(cbc_key, 2, cbc_iv).encrypt(cbc_pt) == cbc_ct)
check("AES-128 CBC decrypt", cryptolib.aes(cbc_key, 2, cbc_iv).decrypt(cbc_ct) == cbc_pt)

# SP 800-38A F.5.1, AES-128 CTR, first two blocks. Mode 6 exists only because
# MICROPY_PY_CRYPTOLIB_CTR is set; it is off by default upstream. The counter
# is incremented by modcryptolib.c and only the block encryption reaches the
# ECDC, so this checks the composition rather than the accelerator.
ctr_key = h("2b7e151628aed2a6abf7158809cf4f3c")
ctr_iv = h("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff")
ctr_pt = h("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51")
ctr_ct = h("874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff")
check("AES-128 CTR encrypt", cryptolib.aes(ctr_key, 6, ctr_iv).encrypt(ctr_pt) == ctr_ct)
check("AES-128 CTR decrypt", cryptolib.aes(ctr_key, 6, ctr_iv).decrypt(ctr_ct) == ctr_pt)

# hashlib, which is mbedtls's md5/sha1/sha256 behind extmod/modhashlib.c. The
# module needs MICROPY_PY_HASHLIB because this port is CORE_FEATURES; the two
# algorithm selections beside it were choosing the contents of a module that
# was never built. Vectors are the standard "abc" digests. There is no sha384
# or sha512: modhashlib.c does not implement them.
import hashlib

check(
    "hashlib md5",
    binascii.hexlify(hashlib.md5(b"abc").digest()) == b"900150983cd24fb0d6963f7d28e17f72",
)
check(
    "hashlib sha1",
    binascii.hexlify(hashlib.sha1(b"abc").digest()) == b"a9993e364706816aba3e25717850c26c9cd0d89d",
)
sha256_abc = b"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
check("hashlib sha256", binascii.hexlify(hashlib.sha256(b"abc").digest()) == sha256_abc)

# update() must accumulate across calls rather than restart the digest.
incremental = hashlib.sha256()
incremental.update(b"a")
incremental.update(b"bc")
check("hashlib sha256 incremental", binascii.hexlify(incremental.digest()) == sha256_abc)

# ssl and requests should both be importable without a filesystem.
import ssl

check("ssl imports", hasattr(ssl, "SSLContext"))
check("ssl has CERT_NONE", hasattr(ssl, "CERT_NONE"))

import requests

check("requests is frozen in", hasattr(requests, "get"))

ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
check("SSLContext constructs", ctx is not None)
ctx.verify_mode = ssl.CERT_NONE
check("verify_mode settable", ctx.verify_mode == ssl.CERT_NONE)

print("%u passed, %u failed" % (passed, failed))
