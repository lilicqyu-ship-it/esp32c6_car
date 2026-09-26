#!/usr/bin/env python3
"""ed25519 reference implementation (RFC 8032 style, pure Python).

Used by the release tooling (dev key generation, bundle signing). The C6
firmware verifies with components/c6_ota/ed25519v.c; the C implementation is
validated against RFC 8032 vectors in test/host/test_ed25519.c, and this
Python module self-tests against the same vectors on import.
"""
import hashlib

p = 2 ** 255 - 19
q = 2 ** 252 + 27742317777372353535851937790883648493


def H(m):
    return hashlib.sha512(m).digest()


def modp_inv(x):
    return pow(x, p - 2, p)


d = (-121665 * modp_inv(121666)) % p


def sha512_modq(s):
    return int.from_bytes(H(s), "little") % q


# points are (x, y) tuples
def point_add(P, Q):
    x1, y1 = P
    x2, y2 = Q
    x3 = (x1 * y2 + x2 * y1) * modp_inv(1 + d * x1 * x2 * y1 * y2)
    y3 = (y1 * y2 + x1 * x2) * modp_inv(1 - d * x1 * x2 * y1 * y2)
    return (x3 % p, y3 % p)


def point_mul(s, P):
    Q = (0, 1)
    while s > 0:
        if s & 1:
            Q = point_add(Q, P)
        P = point_add(P, P)
        s >>= 1
    return Q


def point_equal(P, Q):
    if (P[0] % p) == 0 and (P[1] % p) == 0:
        return (Q[0] % p) == 0 and (Q[1] % p) == 0
    if (Q[0] % p) == 0 and (Q[1] % p) == 0:
        return (P[0] % p) == 0 and (P[1] % p) == 0
    return (P[0] * Q[1] - Q[0] * P[1]) % p == 0


modp_sqrt_m1 = pow(2, (p - 1) // 4, p)


def recover_x(y, sign):
    if y >= p:
        return None
    x2 = (y * y - 1) * modp_inv(d * y * y + 1)
    if x2 == 0:
        if sign:
            return None
        return 0
    x = pow(x2, (p + 3) // 8, p)
    if (x * x - x2) % p != 0:
        x = x * modp_sqrt_m1 % p
    if (x * x - x2) % p != 0:
        return None
    if (x & 1) != sign:
        x = p - x
    return x


g_y = (4 * modp_inv(5)) % p
g_x = recover_x(g_y, 0)
G = (g_x, g_y)


def point_compress(P):
    x = P[0] % p
    y = P[1] % p
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")


def point_decompress(s):
    if len(s) != 32:
        raise ValueError("bad input")
    y = int.from_bytes(s, "little")
    sign = y >> 255
    y &= (1 << 255) - 1
    x = recover_x(y, sign)
    if x is None:
        return None
    return (x, y)


def secret_expand(secret):
    if len(secret) != 32:
        raise ValueError("bad secret")
    h = H(secret)
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= (1 << 254)
    return (a, h[32:])


def secret_to_public(secret):
    (a, _) = secret_expand(secret)
    return point_compress(point_mul(a, G))


def sign(secret, msg):
    a, prefix = secret_expand(secret)
    A = point_compress(point_mul(a, G))
    r = sha512_modq(prefix + msg)
    R = point_mul(r, G)
    Rs = point_compress(R)
    h = sha512_modq(Rs + A + msg)
    s = (r + h * a) % q
    return Rs + int.to_bytes(s, 32, "little")


def verify(public, msg, signature):
    if len(public) != 32:
        raise ValueError("bad public key")
    if len(signature) != 64:
        raise ValueError("bad signature")
    A = point_decompress(public)
    if not A:
        return False
    Rs = signature[:32]
    R = point_decompress(Rs)
    if not R:
        return False
    s = int.from_bytes(signature[32:], "little")
    if s >= q:
        return False
    h = sha512_modq(Rs + public + msg)
    sB = point_mul(s, G)
    hA = point_mul(h, A)
    return point_equal(sB, point_add(R, hA))


# ---- RFC 8032 test vectors ---------------------------------------------------
_VECTORS = [
    (
        "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
        bytes(),
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
        "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
    ),
    (
        "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
        bytes([0x72]),
        "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
        "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
        "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00",
    ),
]


def _selftest():
    for seed_hex, msg, pub_hex, sig_hex in _VECTORS:
        seed = bytes.fromhex(seed_hex)
        pub = secret_to_public(seed)
        assert pub.hex() == pub_hex, "pub mismatch"
        sig = sign(seed, msg)
        assert sig.hex() == sig_hex, "sig mismatch"
        assert verify(bytes.fromhex(pub_hex), msg, bytes.fromhex(sig_hex))
        bad = bytearray(bytes.fromhex(sig_hex))
        bad[0] ^= 1
        assert not verify(bytes.fromhex(pub_hex), msg, bytes(bad))
    return True


if __name__ == "__main__":
    _selftest()
    print("ed25519_ref: RFC 8032 vectors OK")
