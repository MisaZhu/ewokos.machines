#!/usr/bin/env python3
"""结构校验 ewokos.iso: PVD / El Torito catalog / 目录记录 / 文件内容对齐"""
import struct, sys
from pathlib import Path

iso_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("ewokos.iso")
iso = iso_path.read_bytes()
B = 2048
errors = []

def chk(cond, msg):
    print(("PASS " if cond else "FAIL ") + msg)
    if not cond:
        errors.append(msg)

def le32(off):  return struct.unpack("<I", iso[off:off+4])[0]
def be32(off):  return struct.unpack(">I", iso[off:off+4])[0]
def le16(off):  return struct.unpack("<H", iso[off:off+2])[0]

# --- Volume descriptors ---
pvd = 16 * B
chk(iso[pvd:pvd+1] == b"\x01" and iso[pvd+1:pvd+6] == b"CD001", "PVD type=1 CD001 @LBA16")
chk(le32(pvd+128) == 2048, f"PVD logical block size 2048 (={le32(pvd+128)})")
vol_size = le32(pvd+80)
chk(vol_size * B == len(iso), f"PVD volume size {vol_size} blocks == file size {len(iso)//B} blocks")
chk(be32(pvd+84) == vol_size, "PVD volume size BE matches")
root_ext = le32(pvd+156+2)
chk(root_ext == 22, f"PVD root dir record extent=22 (={root_ext})")

brvd = 17 * B
chk(iso[brvd+1:brvd+6] == b"CD001" and iso[brvd+7:brvd+30] == b"EL TORITO SPECIFICATION",
    "Boot Record VD 'EL TORITO SPECIFICATION' @LBA17")
catalog_lba = le32(brvd+71)
chk(catalog_lba == 19, f"BRVD catalog LBA=19 (={catalog_lba})")

term = 18 * B
chk(iso[term] == 255 and iso[term+1:term+6] == b"CD001", "VD set terminator type=255 @LBA18")

# --- El Torito catalog ---
cat = 19 * B
ve = iso[cat:cat+32]
chk(ve[0] == 0x01 and ve[1] == 0x00 and ve[30:32] == b"\x55\xaa",
    "validation entry: header id 0x01, platform 0x00, 0x55AA")
wsum = sum(struct.unpack("<16H", ve)) & 0xFFFF
chk(wsum == 0, f"validation entry word-sum == 0 (={wsum:#06x})")
de = iso[cat+32:cat+64]
stage1_lba = struct.unpack("<I", de[8:12])[0]
sect_cnt = le16(cat+32+6)
chk(de[0] == 0x88 and de[1] == 0x00, f"default entry bootable, no-emulation (media={de[1]})")
chk(stage1_lba == 24, f"default entry load RBA=24 (={stage1_lba})")
print(f"       default entry: load_seg={le16(cat+32+2):#06x} sector_count={sect_cnt} (装载 {sect_cnt*512}B)")
sh = iso[cat+64:cat+96]
chk(sh[0] == 0x91 and sh[1] == 0xEF, "section header 0x91 platform 0xEF (UEFI)")
ee = iso[cat+96:cat+128]
esp_lba = struct.unpack("<I", ee[8:12])[0]
esp_sect = le16(cat+96+6)
chk(ee[0] == 0x88 and ee[1] == 0x00 and ee[4] == 0xEF,
    f"EFI entry bootable no-emulation sys_type=0xEF, esp@{esp_lba} sectors={esp_sect}")

# --- path tables / root dir ---
pt = 20 * B
chk(pt and iso[pt] == 1 and struct.unpack("<I", iso[pt+2:pt+6])[0] == 22,
    "type-L path table: single root record -> LBA22")
rd = 22 * B
names = {}
off = rd
while iso[off] > 0:
    rl = iso[off]
    name = iso[off+33:off+33+iso[off+32]]
    names[name] = (le32(off+2), le32(off+10))
    off += rl
    if iso[off] == 0 and (off - rd) % 2:
        off += 1
        if iso[off] == 0:
            break
chk(b"\x00" in names and b"\x01" in names, "root dir has '.' and '..' records")
for expect in [b"KERNEL.ELF;1", b"KERNEL.IMG;1", b"ROOTFS.IMG;1"]:
    if expect in names:
        e, sz = names[expect]
        chk(e >= 23 and sz > 0, f"{expect.decode()} 目录记录有效 (LBA={e} size={sz})")
    else:
        chk(False, f"root dir missing {expect.decode()}")

# --- file content spot checks ---
kernel_elf = Path("kernel.elf").read_bytes()
ko, ks = names[b"KERNEL.ELF;1"]
chk(iso[ko*B:ko*B+len(kernel_elf)] == kernel_elf, "KERNEL.ELF content matches kernel.elf")
ki, ks2 = names[b"KERNEL.IMG;1"]
kimg = Path("kernel.img").read_bytes()
chk(iso[ki*B:ki*B+len(kimg)] == kimg, "KERNEL.IMG content matches kernel.img")
ro, rs = names[b"ROOTFS.IMG;1"]
rfs = Path("../system/root_x86_iso.img").read_bytes()
chk(rs == len(rfs), f"ROOTFS.IMG size matches ({rs} == {len(rfs)})")
chk(iso[ro*B:ro*B+min(4096, len(rfs))] == rfs[:4096], "ROOTFS.IMG first 4KB matches root_x86_iso.img")
esp = Path("boot/uefi/esp_iso.img").read_bytes()
chk(iso[esp_lba*B:esp_lba*B+len(esp)] == esp, "ESP image content matches esp_iso.img")
# system area: MBR 补丁槽 + 内核拷贝(被 PVD 截断, 仅容量参考)
chk(iso[510:512] == b"\x55\xaa", "LBA0 has 0x55AA (isohybrid MBR)")
magic = struct.pack("<I", 0x4B4F5745)  # "EWOK"
kernel_sector = ki * 4
chk(iso[:446].find(magic) < 0, "kernel_start_lba slot patched (no EWOK magic left in MBR)")
hits = iso[:446].count(struct.pack("<I", kernel_sector))
chk(hits == 1, f"MBR kernel_start_lba patched to KERNEL.IMG 512B LBA {kernel_sector}")
p1_start = le32(446 + 8)
p1_cnt = le32(446 + 12)
chk(p1_start == kernel_sector and p1_cnt >= len(kimg) // 512,
    f"p1 start={p1_start} count={p1_cnt} -> KERNEL.IMG @sector {kernel_sector}")

# stage1: LBA24 处应为补丁后的 cd_stage1 (1CDK 槽 -> 内核 2KB LBA)
stage1 = bytearray(Path("boot/cd_stage1.bin").read_bytes())
import struct as _s
i = stage1.find(_s.pack("<I", 0x4B444331))
if i >= 0:
    stage1[i:i+4] = _s.pack("<I", ki)   # ki = KERNEL.IMG 的 2KB LBA
stage1 = bytes(stage1)
chk(len(stage1) <= B and iso[24*B:24*B+len(stage1)] == stage1,
    "cd_stage1.bin (已补丁 kcdl) @LBA24")
need_sc = (B + len(kimg) + 511) // 512
sc = le16(cat+32+6)
chk(sc >= need_sc, f"catalog sector_count {sc} covers stage1-block+kernel ({need_sc})")

print()
print("ALL OK" if not errors else f"{len(errors)} FAILURES")
sys.exit(1 if errors else 0)
