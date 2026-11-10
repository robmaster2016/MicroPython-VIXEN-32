# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
# Crypto building blocks used by TLS and sshd: SHA-256, AES-CTR, os.urandom,
# X25519 (RFC 7748) and Ed25519 (RFC 8032). Vectors cross-checked with OpenSSL.
# Runs offline. Firmware: build-net or build-lvgl-net.
import binascii, hashlib, cryptolib, os, curve25519

h = binascii.unhexlify
fails = 0


def check(name, got, want):
    global fails
    if got == want:
        print("ok  ", name)
    else:
        fails += 1
        print("FAIL", name, binascii.hexlify(got) if isinstance(got, bytes) else got)


check("sha256 abc", hashlib.sha256(b"abc").digest(),
      h("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"))
d = hashlib.sha256()
for _ in range(100):
    d.update(b"x" * 1000)
check("sha256 100k", d.digest(), hashlib.sha256(b"x" * 100000).digest())

# NIST SP 800-38A F.5.1, AES-128-CTR, also split across a block boundary
key = h("2b7e151628aed2a6abf7158809cf4f3c")
ctr = h("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff")
pt = h("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51")
ct = h("874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff")
check("aes-ctr", cryptolib.aes(key, cryptolib.MODE_CTR, ctr).encrypt(pt), ct)
a = cryptolib.aes(key, cryptolib.MODE_CTR, ctr)
check("aes-ctr split", a.encrypt(pt[:5]) + a.encrypt(pt[5:20]) + a.encrypt(pt[20:]), ct)
check("aes-ctr decrypt", cryptolib.aes(key, cryptolib.MODE_CTR, ctr).decrypt(ct), pt)

r = [os.urandom(32) for _ in range(8)]
check("urandom length", len(r[0]), 32)
check("urandom distinct", len(set(r)), 8)

# RFC 7748 5.2 and 6.1
check("x25519 #1", curve25519.x25519(
    h("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"),
    h("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c")),
    h("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"))
check("x25519 #2", curve25519.x25519(
    h("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d"),
    h("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493")),
    h("95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957"))
nine = b"\x09" + bytes(31)
check("x25519 1 iteration", curve25519.x25519(nine, nine),
      h("422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079"))
alice = h("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a")
bob_pub = h("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f")
check("x25519 public", curve25519.x25519(alice),
      h("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"))
check("x25519 shared", curve25519.x25519(alice, bob_pub),
      h("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"))
try:
    curve25519.x25519(alice, bytes(32))
    check("x25519 zero point rejected", False, True)
except ValueError:
    check("x25519 zero point rejected", True, True)

# RFC 8032 7.1 tests 2 and 3, plus a longer message signed by OpenSSL
ED = (
    ("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
     "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", b"\x72",
     "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
     "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"),
    ("c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
     "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", b"\xaf\x82",
     "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
     "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"),
    ("833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42",
     "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
     b"VIXEN-32 feels the magic of MicroPython",
     "8129d059e52d383d720156116f3596c5cbc190a7fbf21a3a2f1ffa18bcb81a7c"
     "ae183489b73363c405d77fce1965ce441aadeed74704c4af415af0150a4e8000"),
)
for i, (seed, pub, msg, sig) in enumerate(ED):
    seed, pub, sig = h(seed), h(pub), h(sig)
    check("ed25519 public %d" % i, curve25519.ed25519_public(seed), pub)
    check("ed25519 sign %d" % i, curve25519.ed25519_sign(seed, msg), sig)
    check("ed25519 sign+pub %d" % i, curve25519.ed25519_sign(seed, msg, pub), sig)
    check("ed25519 verify %d" % i, curve25519.ed25519_verify(pub, msg, sig), True)
    bad = bytearray(sig)
    bad[5] ^= 1
    check("ed25519 reject %d" % i, curve25519.ed25519_verify(pub, msg, bytes(bad)), False)
    check("ed25519 wrong msg %d" % i, curve25519.ed25519_verify(pub, msg + b"!", sig), False)

seed = os.urandom(32)
pub = curve25519.ed25519_public(seed)
check("ed25519 empty msg", curve25519.ed25519_verify(pub, b"", curve25519.ed25519_sign(seed, b"")), True)

print("TEST OK" if not fails else "TEST FAILED (%d)" % fails)
