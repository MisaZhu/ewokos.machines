#!/usr/bin/env python3
"""mkpefi.py <in.elf> <out.efi> <subsystem>
把 ELF64 链接产物转成 UEFI PE32+ 镜像。
binutils 的 objcopy -O pe-x86-64 产出的是无 MZ/PE 头的裸 COFF, EDK2 无法加载,
故自行生成完整 PE32+。要求代码 RIP 相对寻址(-fPIE, 无绝对重定位),
DataDirectory 全空 → EDK2 按无重定位镜像处理, 可加载到任意基址。
"""
import struct, sys

FILE_ALIGN = 0x200
SEC_ALIGN = 0x1000

SHT_PROGBITS, SHT_NOBITS = 1, 8
SHF_ALLOC, SHF_EXEC, SHF_WRITE = 0x2, 0x4, 0x1

PE_NAMES = {".text": ".text", ".rodata": ".rdata", ".data": ".data", ".bss": ".bss"}

def align(v, a):
    return (v + a - 1) & ~(a - 1)

def read_elf(path):
    data = open(path, "rb").read()
    assert data[:4] == b"\x7fELF" and data[4] == 2, "need ELF64"
    e_entry = struct.unpack_from("<Q", data, 0x18)[0]
    e_shoff = struct.unpack_from("<Q", data, 0x28)[0]
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    secs = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        f = struct.unpack_from("<IIQQQQIIQQ", data, o)
        secs.append(dict(name_off=f[0], type=f[1], flags=f[2], addr=f[3],
                         off=f[4], size=f[5]))
    shstr = secs[e_shstrndx]
    for s in secs:
        end = data.index(b"\0", shstr["off"] + s["name_off"])
        s["name"] = data[shstr["off"] + s["name_off"]:end].decode()
    return data, e_entry, secs

def main():
    elf_path, out_path, subsystem = sys.argv[1], sys.argv[2], int(sys.argv[3])
    data, entry, secs = read_elf(elf_path)

    keep = []
    for s in secs:
        if s["type"] not in (SHT_PROGBITS, SHT_NOBITS):
            continue
        if not (s["flags"] & SHF_ALLOC) or s["size"] == 0:
            continue
        if s["name"].startswith((".note", ".comment", ".eh_frame")):
            continue
        keep.append(s)
    keep.sort(key=lambda s: s["addr"])
    assert keep, "no loadable sections"
    for s in keep:
        assert s["addr"] % SEC_ALIGN == 0, f"{s['name']} vaddr {s['addr']:#x} not page aligned"

    n = len(keep)
    pe_off = 0x80
    opt_off = pe_off + 4 + 20
    sect_off = opt_off + 240

    # .reloc: 一个合法但无实际项的重定位块(ABSOLUTE=nop 项)。
    # EDK2 对无 .reloc 的镜像按 "RelocationsStripped" 走另一条加载/保护路径,
    # OVMF 会把整镜 RO 保护, 间接破坏固件自身页属性 → 必须提供 .reloc。
    # 代码为 -fPIE RIP 相对寻址, 本身无绝对地址需要修正。
    reloc_page = keep[0]["addr"]
    reloc_data = struct.pack("<II", reloc_page, 12) + struct.pack("<HH",
                (0 << 12) | 0,   # type 0 = IMAGE_REL_BASED_ABSOLUTE (nop)
                (0 << 12) | 0)
    n += 1  # 多一个 .reloc 段

    hdr_size = align(sect_off + 40 * n, FILE_ALIGN)

    # ---- 段布局 ----
    sec_meta, raw_off, max_end = [], hdr_size, 0
    for s in keep:
        if s["type"] == SHT_NOBITS:
            sec_meta.append((s, 0, 0))
        else:
            sec_meta.append((s, raw_off, s["size"]))
            raw_off += align(s["size"], FILE_ALIGN)
        max_end = max(max_end, s["addr"] + s["size"])
    # 追加 .reloc 段(虚拟地址对齐到下一页)
    reloc_rva = align(max_end, SEC_ALIGN)
    sec_meta.append(("__reloc__", raw_off, len(reloc_data)))
    raw_off += align(len(reloc_data), FILE_ALIGN)
    max_end = reloc_rva + len(reloc_data)
    size_of_image = align(max_end, SEC_ALIGN)
    # SizeOfHeaders = 仅头区(DOS+PE+段表, 对齐到 FileAlignment), 不是文件总长!
    # EDK2 会校验 每段 PointerToRawData >= SizeOfHeaders, 写错直接 Unsupported
    size_of_headers = hdr_size

    # 注意: 必须预分配完整文件大小。bytearray 对越界切片赋值会在"末尾追加"
    # 而不是按偏移写入(留空洞), 会把段数据全部挤到文件尾部 → PE 非法。
    out = bytearray(raw_off)

    # ---- DOS 头 ----
    out[0:2] = b"MZ"
    struct.pack_into("<I", out, 0x3C, pe_off)

    # ---- PE 签名 + COFF 头 ----
    out[pe_off:pe_off + 4] = b"PE\x00\x00"
    struct.pack_into("<HHIIIHH", out, pe_off + 4,
                     0x8664, n, 0, 0, 0, 240,
                     0x22)  # EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE

    # ---- Optional header (PE32+, magic 0x20B) ----
    o = opt_off
    real_secs = [m for m in sec_meta if m[0] != "__reloc__"]
    code_sz = sum(m[2] for m in real_secs if m[0]["flags"] & SHF_EXEC)
    init_sz = sum(m[2] for m in real_secs if not (m[0]["flags"] & SHF_EXEC)
                  and m[0]["type"] != SHT_NOBITS)
    uninit_sz = sum(m[0]["size"] for m in real_secs if m[0]["type"] == SHT_NOBITS)
    struct.pack_into("<H", out, o, 0x20B)
    out[o + 2] = 1            # linker ver
    struct.pack_into("<III", out, o + 4, code_sz, init_sz, uninit_sz)
    struct.pack_into("<I", out, o + 16, entry)          # AddressOfEntryPoint
    struct.pack_into("<I", out, o + 20, keep[0]["addr"])  # BaseOfCode
    struct.pack_into("<Q", out, o + 24, 0x10000)        # ImageBase
    struct.pack_into("<II", out, o + 32, SEC_ALIGN, FILE_ALIGN)
    struct.pack_into("<HHHHHH", out, o + 40, 6, 0, 0, 0, 6, 0)  # OS/Image/Subsys ver
    struct.pack_into("<IIII", out, o + 52, 0, size_of_image, size_of_headers, 0)  # Win32Ver/SizeImage/SizeHdr/CheckSum
    struct.pack_into("<H", out, o + 68, subsystem)      # Subsystem
    struct.pack_into("<H", out, o + 70, 0)              # DllCharacteristics
    struct.pack_into("<QQQQ", out, o + 72, 0x100000, 0x1000, 0, 0)  # stacks/heaps
    struct.pack_into("<II", out, o + 104, 0, 16)        # LoaderFlags, NumberOfRvaAndSizes
    struct.pack_into("<II", out, o + 112 + 5 * 8, reloc_rva, len(reloc_data))  # DataDirectory[5]=.reloc

    # ---- 段表 ----
    for i, (s, roff, rsz) in enumerate(sec_meta):
        so = sect_off + 40 * i
        if s == "__reloc__":
            name, s_addr, s_size, s_flags = ".reloc", reloc_rva, len(reloc_data), None
        else:
            name = PE_NAMES.get(s["name"], s["name"][:8])
            s_addr, s_size, s_flags, s_type = s["addr"], s["size"], s["flags"], s["type"]
        out[so:so + len(name)] = name.encode()
        struct.pack_into("<IIII", out, so + 8,
                         s_size,                 # VirtualSize
                         s_addr,                 # VirtualAddress (RVA)
                         rsz,                    # SizeOfRawData
                         roff)                   # PointerToRawData
        if s != "__reloc__":
            if s_flags & SHF_EXEC:
                ch = 0x60000020
            elif s_type == SHT_NOBITS:
                ch = 0xC0000080
            else:
                ch = 0x40000040
        else:
            ch = 0x42000040                      # INIT_DATA | READ | DISCARDABLE
        struct.pack_into("<I", out, so + 36, ch)
        if rsz:
            if s == "__reloc__":
                out[roff:roff + rsz] = reloc_data
            else:
                out[roff:roff + rsz] = data[s["off"]:s["off"] + s["size"]]

    open(out_path, "wb").write(out)
    print(f"mkpefi: {out_path} subsys={subsystem} secs="
          + ",".join(f"{(m[0] if isinstance(m[0], str) else m[0]['name'])}@{m[1]:#x}" for m in sec_meta)
          + f" entry={entry:#x} size={size_of_image:#x}")

if __name__ == "__main__":
    main()
