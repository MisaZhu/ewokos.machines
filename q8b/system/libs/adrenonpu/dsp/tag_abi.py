"""Tag a locally linked Hexagon ELF with the QuRT loader's ABI version."""
import struct
import sys
from pathlib import Path

path = Path(sys.argv[1])
data = bytearray(path.read_bytes())
if data[:7] != b"\x7fELF\x01\x01\x01" or struct.unpack_from("<H", data, 18)[0] != 164:
    raise ValueError("expected little-endian Hexagon ELF32")
phoff = struct.unpack_from("<I", data, 28)[0]
phsize, count = struct.unpack_from("<HH", data, 42)
if phsize != 32:
    raise ValueError("unexpected program header size")
changed = 0
for i in range(count):
    ph = struct.unpack_from("<8I", data, phoff + i * phsize)
    if ph[0] != 2:
        continue
    for offset in range(ph[1], ph[1] + ph[4], 8):
        if struct.unpack_from("<I", data, offset)[0] == 14:
            # Reuse the optional SONAME slot without moving any ELF tables.
            struct.pack_into("<II", data, offset, 0x70000001, 3)
            changed += 1
if changed != 1:
    raise ValueError("expected exactly one SONAME slot for the QuRT ABI tag")
path.write_bytes(data)
