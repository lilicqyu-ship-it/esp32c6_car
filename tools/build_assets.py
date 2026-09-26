#!/usr/bin/env python3
"""Pack assets_src/ into build/assets.bin for the assets partition.

Layout (components/c6_http/assets_store.c parses this):
    header 16B:  "ASSETS" | ver=1 | rsv | count u16 | total u32 | pad
    entry  32B:  name[16] | off u32 | gz_len u32 | raw_len u32 | crc32 u32
    payload:     gzip streams (raw CRC over the *raw* bytes)

Usage:  python tools/build_assets.py [assets_src] [out]
        then flash:  parttool.py --port PORT write_partition --partition-name=assets \
                     --input build/assets.bin
"""
import gzip
import os
import struct
import sys
import zlib

ALIGN = 4
HDR = struct.Struct("<6sBBHI2x")
ENT = struct.Struct("<16sIIII")


def pack(src, out):
    files = sorted(
        f for f in os.listdir(src)
        if os.path.isfile(os.path.join(src, f)) and not f.startswith(".")
    )
    if not files:
        print("no assets in", src)
        return 1
    if len(files) > 16:
        print("too many assets (max 16)")
        return 1

    blobs = []
    entries = []
    off = HDR.size + ENT.size * len(files)
    for name in files:
        raw = open(os.path.join(src, name), "rb").read()
        gz = gzip.compress(raw, 9, mtime=0)
        if len(gz) >= len(raw):
            gz = raw                            # store uncompressed if bigger
        pad = (-len(gz)) % ALIGN
        gz += b"\x00" * pad
        entries.append((name.encode(), off, len(gz), len(raw), zlib.crc32(raw)))
        blobs.append(gz)
        off += len(gz)

    total = off
    with open(out, "wb") as f:
        f.write(HDR.pack(b"ASSETS", 1, 0, len(files), total))
        for name, e_off, gz_len, raw_len, crc in entries:
            f.write(ENT.pack(name.ljust(16, b"\x00")[:16], e_off, gz_len, raw_len, crc))
        for b in blobs:
            f.write(b)
    print("wrote %s (%d files, %d bytes)" % (out, len(files), total))
    return 0


if __name__ == "__main__":
    src = sys.argv[1] if len(sys.argv) > 1 else "assets_src"
    out = sys.argv[2] if len(sys.argv) > 2 else "build/assets.bin"
    sys.exit(pack(src, out))
