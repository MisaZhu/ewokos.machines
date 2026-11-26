#!/usr/bin/env python3
"""Pack a file as a raw LZ4 block with a 12-byte OLZ4 header:

    magic "OLZ4" | u32 LE uncompressed size | u32 LE block size | block

The bootloader (efi/main.c lz4_decompress) inflates it back to the plain
image before the kernel handoff.  Greedy hash-chain matcher, stdlib only.
"""
import struct
import sys


def lz4_block(data):
    n = len(data)
    out = bytearray()
    ht = {}
    i = 0
    anchor = 0
    limit = n - 5           # LZ4: the last 5 bytes are always literals

    def emit(lit_start, lit_end, match_off, match_len):
        lit = lit_end - lit_start
        ml = match_len - 4
        out.append(((15 if lit >= 15 else lit) << 4) |
                   (15 if ml >= 15 else ml))
        if lit >= 15:
            l = lit - 15
            while l >= 255:
                out.append(255)
                l -= 255
            out.append(l)
        out.extend(data[lit_start:lit_end])
        out.append(match_off & 0xFF)
        out.append(match_off >> 8)
        if ml >= 15:
            l = ml - 15
            while l >= 255:
                out.append(255)
                l -= 255
            out.append(l)

    while i < limit:
        key = data[i:i + 4]
        j = ht.get(key, -1)
        ht[key] = i
        if j >= 0 and i - j < 65536 and data[j:j + 4] == key:
            ml = 4
            while i + ml < limit and data[j + ml] == data[i + ml]:
                ml += 1
            emit(anchor, i, i - j, ml)
            i += ml
            anchor = i
        else:
            i += 1

    lit = n - anchor
    out.append((15 if lit >= 15 else lit) << 4)
    if lit >= 15:
        l = lit - 15
        while l >= 255:
            out.append(255)
            l -= 255
        out.append(l)
    out.extend(data[anchor:])
    return bytes(out)


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: mklz4.py <input> <output.lz4>")
    with open(sys.argv[1], "rb") as f:
        data = f.read()
    block = lz4_block(data)
    with open(sys.argv[2], "wb") as f:
        f.write(b"OLZ4" + struct.pack("<II", len(data), len(block)) + block)
    print("mklz4: %s %d -> %d bytes (%.1f%%)" %
          (sys.argv[2], len(data), len(block) + 12,
           100.0 * (len(block) + 12) / max(1, len(data))))


if __name__ == "__main__":
    main()