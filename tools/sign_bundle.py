#!/usr/bin/env python3
"""Build a signed C6 firmware bundle for POST /ota/c6 (LLDD 4.7).

Bundle layout (components/c6_ota/bundle.c parses this; all fields LE):
    magic "C6FW" | fmt=1 | flags | hdr_len=148 u16 | total u32
    c6_len u32 | c6_sha256 32B | assets_len u32 | assets_sha256 32B
    ed25519 sig 64B over the first 84 bytes
    payload: c6.bin [+ assets.bin]

Usage:
    python tools/sign_bundle.py --c6 build/c6_car.bin \
        [--assets build/assets.bin] \
        [--seed-file tools/keys/ed25519_dev.seed] \
        --out build/c6fw.bundle
"""
import argparse
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ed25519_ref as ed  # noqa: E402

HDR_LEN = 148
SIGNED_LEN = 84


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--c6", required=True, help="bootloader-app .bin image")
    ap.add_argument("--assets", help="optional assets.bin (written to assets partition)")
    ap.add_argument("--seed-file", default="tools/keys/ed25519_dev.seed")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    c6 = open(args.c6, "rb").read()
    assets = open(args.assets, "rb").read() if args.assets else b""
    if not assets:
        # default: pack assets_src if the file was not built yet
        if os.path.exists("assets_src"):
            import subprocess
            os.makedirs("build", exist_ok=True)
            subprocess.check_call([sys.executable, "tools/build_assets.py",
                                   "assets_src", "build/assets.bin"])
            assets = open("build/assets.bin", "rb").read()
    if len(c6) > 3 * 1024 * 1024 or len(assets) > 512 * 1024:
        print("payload too large")
        return 1

    seed = bytes.fromhex(open(args.seed_file).read().strip())
    pub = ed.secret_to_public(seed)

    flags = 1 if assets else 0
    total = HDR_LEN + len(c6) + len(assets)
    head = bytearray()
    head += b"C6FW"
    head += struct.pack("<BBH", 1, flags, HDR_LEN)
    head += struct.pack("<I", total)
    head += struct.pack("<I", len(c6))
    head += hashlib.sha512(c6).digest()[:32]
    head += struct.pack("<I", len(assets))
    head += hashlib.sha512(assets).digest()[:32] if assets else bytes(32)
    assert len(head) == SIGNED_LEN, len(head)

    sig = ed.sign(seed, bytes(head))
    out = bytes(head) + sig + c6 + assets
    open(args.out, "wb").write(out)
    print("bundle %s: %d bytes (c6 %d, assets %d) signed pub=%s..." %
          (args.out, len(out), len(c6), len(assets), pub.hex()[:16]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
