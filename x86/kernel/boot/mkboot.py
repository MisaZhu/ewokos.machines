#!/usr/bin/env python3
from pathlib import Path
import re
import struct
import subprocess
import sys


FLOPPY_144_SIZE = 1474560
SECTOR_SIZE = 512
MBR_CODE_SIZE = 446
# bios_boot.S kernel_start_lba 槽 (MBR 引导代码读内核的起始 LBA):
# 链接期填 "EWOK" 魔数, 这里按镜像类型补丁 (hdd/floppy=1, ISO=KERNEL.IMG 文件区)
KERNEL_LBA_MAGIC = 0x4B4F5745
# cd_stage1.S kcdl_slot (BIOS 截断 no-emulation 装载时的 INT13h CD 回退装载):
# 链接期填 "1CDK" 魔数, mkboot 补丁为内核的 2KB LBA (文件区 KERNEL.IMG)
KCDL_MAGIC = 0x4B444331


def patch_slot(data, magic_int, value, what="patch slot"):
    off = bytes(data).find(struct.pack("<I", magic_int))
    if off < 0:
        raise SystemExit(f"{what} ({magic_int:#010x}) not found")
    data[off:off + 4] = struct.pack("<I", value)


def patch_kernel_lba(mbr, start_sector):
    """把 MBR 引导代码的内核起始 LBA 槽补丁为 start_sector"""
    patch_slot(mbr, KERNEL_LBA_MAGIC, start_sector, "kernel_start_lba slot")
ROOTFS_ALIGN_SECTORS = 2048


def gen_cfg(argv):
    if len(argv) != 5:
        raise SystemExit("usage: mkboot.py cfg LOAD_ADDRESS READELF ELF IMG")

    load_address = int(argv[1], 16)
    readelf = argv[2]
    elf_path = Path(argv[3])
    img_path = Path(argv[4])

    image_size = img_path.stat().st_size
    image = img_path.read_bytes()
    kernel_sectors = (image_size + 511) // 512
    header = subprocess.check_output([readelf, "-h", str(elf_path)], text=True)
    match = re.search(r"Entry point address:\s+(\S+)", header)
    if match is None:
        raise SystemExit("failed to parse entry point from readelf output")
    entry = int(match.group(1), 16)
    entry_offset = entry - load_address
    if entry_offset < 0 or entry_offset + 4 > len(image):
        raise SystemExit("kernel entry offset is outside kernel image")
    entry_magic = int.from_bytes(image[entry_offset:entry_offset + 4], "little")

    print(f".equ KERNEL_SECTORS, {kernel_sectors}")
    print(f".equ KERNEL_ENTRY_OFFSET, 0x{entry_offset:x}")
    print(f".equ KERNEL_ENTRY_MAGIC, 0x{entry_magic:08x}")


def build_image(argv):
    if len(argv) != 4:
        raise SystemExit("usage: mkboot.py image BOOT_BIN KERNEL_IMG OUT")

    boot_path = Path(argv[1])
    kernel_path = Path(argv[2])
    out_path = Path(argv[3])

    boot = boot_path.read_bytes()
    kernel = kernel_path.read_bytes()
    if len(boot) != 512:
        raise SystemExit(f"boot sector size mismatch: {len(boot)}")

    image = boot + kernel
    image = bytearray(image)
    patch_kernel_lba(image, 1)     # 扇区 1 起 = 内核裸镜像
    if len(image) > FLOPPY_144_SIZE:
        raise SystemExit(f"boot image too large for 1.44MB floppy: {len(image)} bytes")
    padding = FLOPPY_144_SIZE - len(image)
    out_path.write_bytes(image + (b"\0" * padding))


def align_up(value, align):
    return ((value + align - 1) // align) * align


def chs_fields(lba):
    """LBA → (cyl, head, sector), 255头/63扇区/1024柱几何"""
    head_per_cyl = 255
    sect_per_cyl = 63
    cyl = lba // (head_per_cyl * sect_per_cyl)
    rem = lba % (head_per_cyl * sect_per_cyl)
    head = rem // sect_per_cyl
    sect = rem % sect_per_cyl + 1
    if cyl > 1023:
        cyl = 1023
        head = 254
        sect = 63
    return cyl, head, sect


def build_partition_entry(start_sector, sector_count, part_type=0x83, bootable=False,
                          end_chs=None):
    entry = bytearray(16)
    if bootable:
        entry[0] = 0x80
    else:
        entry[0] = 0x00
    bc, bh, bs = chs_fields(start_sector)
    entry[1] = bh
    entry[2] = ((bc >> 8) << 6) | bs       # bits 5-0 sector, bits 7-6 cyl high
    entry[3] = bc & 0xFF
    entry[4] = part_type
    if end_chs is None:
        ec, eh, es = chs_fields(max(start_sector + sector_count - 1, start_sector))
    else:
        ec, eh, es = end_chs
    entry[5] = eh
    entry[6] = ((ec >> 8) << 6) | es
    entry[7] = ec & 0xFF
    entry[8:12] = start_sector.to_bytes(4, "little")
    entry[12:16] = sector_count.to_bytes(4, "little")
    return bytes(entry)


def build_hdd(argv):
    # 可选第 6 参 ESP 镜像: 提供时生成 BIOS/UEFI 双路径磁盘:
    #   扇区 0     MBR(引导扇区代码 + p1: FAT32 ESP(0x0C) + p2: ext3 rootfs(0x83))
    #   扇区 1 起  内核裸镜像 (BIOS 路径, INT13h 直读)
    #   扇区 2048  ESP (/EFI/BOOT/BOOTX64.EFI + /KERNEL.ELF, UEFI 路径)
    #   其后       ext3 rootfs (分区表中最后一个非空分区, 内核按此识别 rootfs)
    if len(argv) not in (5, 6):
        raise SystemExit(
            "usage: mkboot.py hdd BOOT_BIN KERNEL_IMG ROOTFS_IMG OUT [ESP_IMG]"
        )

    boot_path = Path(argv[1])
    kernel_path = Path(argv[2])
    rootfs_path = Path(argv[3])
    out_path = Path(argv[4])
    esp_path = Path(argv[5]) if len(argv) == 6 else None

    boot = boot_path.read_bytes()
    kernel = kernel_path.read_bytes()
    rootfs = rootfs_path.read_bytes()
    if len(boot) != SECTOR_SIZE:
        raise SystemExit(f"boot sector size mismatch: {len(boot)}")

    kernel_sectors = (len(kernel) + SECTOR_SIZE - 1) // SECTOR_SIZE
    rootfs_sectors = (len(rootfs) + SECTOR_SIZE - 1) // SECTOR_SIZE

    esp_start = ROOTFS_ALIGN_SECTORS
    if esp_path is not None:
        esp = esp_path.read_bytes()
        esp_sectors = (len(esp) + SECTOR_SIZE - 1) // SECTOR_SIZE
        rootfs_start = align_up(esp_start + esp_sectors, ROOTFS_ALIGN_SECTORS)
    else:
        esp = None
        esp_sectors = 0
        rootfs_start = max(
            ROOTFS_ALIGN_SECTORS,
            align_up(1 + kernel_sectors, ROOTFS_ALIGN_SECTORS),
        )
    disk_sectors = rootfs_start + rootfs_sectors

    disk = bytearray(disk_sectors * SECTOR_SIZE)
    disk[:MBR_CODE_SIZE] = boot[:MBR_CODE_SIZE]
    patch_kernel_lba(disk, 1)      # 内核裸镜像在扇区 1 起
    if esp is not None:
        disk[MBR_CODE_SIZE:MBR_CODE_SIZE + 16] = build_partition_entry(
            esp_start, esp_sectors, part_type=0x0C, bootable=True
        )
        disk[MBR_CODE_SIZE + 16:MBR_CODE_SIZE + 32] = build_partition_entry(
            rootfs_start, rootfs_sectors
        )
    else:
        disk[MBR_CODE_SIZE:MBR_CODE_SIZE + 16] = build_partition_entry(
            rootfs_start, rootfs_sectors
        )
    disk[510:512] = b"\x55\xaa"

    kernel_offset = SECTOR_SIZE
    disk[kernel_offset:kernel_offset + len(kernel)] = kernel

    if esp is not None:
        esp_offset = esp_start * SECTOR_SIZE
        disk[esp_offset:esp_offset + len(esp)] = esp

    rootfs_offset = rootfs_start * SECTOR_SIZE
    disk[rootfs_offset:rootfs_offset + len(rootfs)] = rootfs

    out_path.write_bytes(disk)


# ---------------------------------------------------------------------------
# ISO9660 + El Torito 可引导 ISO (不依赖 xorriso/genisoimage)
#
# LBA(2048B) 布局:
#   0-15   system area: 字节0=MBR(446B 引导码+0x55AA, kernel_start_lba 槽
#          指向文件区 KERNEL.IMG), 字节512起=内核拷贝 (仅供容量参考, 被
#          PVD@16 截断, 不可引导)
#   16     PVD                    17  Boot Record VD (El Torito)
#   18     VD set terminator      19  boot catalog
#   20/21  type-L / type-M 路径表 (仅根记录)
#   22     根目录
#   23+    文件区 (stage1/KERNEL.IMG/KERNEL.ELF/ROOTFS.IMG/ESP, 各 2048
#          对齐); MBR/GRUB-chainload/U盘路径经 kernel_start_lba 槽读文件
#          区的完整内核拷贝
# ---------------------------------------------------------------------------

ISO_BLOCK = 2048
PVD_LBA = 16
BRVD_LBA = 17
TERM_LBA = 18
CATALOG_LBA = 19
PATH_TABLE_LBA = 20
ROOT_DIR_LBA = 22
FILES_LBA = 23
ISO_DATE = bytes([126, 9, 30, 0, 0, 0, 0])          # 记录时间 (GMT+0)
ISO_LONG_DATE = b"2026093000000000\x00"              # 卷日期


def iso_dir_record(extent, size, flags, name):
    """ISO9660 目录记录 (长度补偶; '.'/'..' 用 0x00/0x01 标识符)"""
    rec_len = 33 + len(name)
    if rec_len % 2:
        rec_len += 1
    rec = bytearray(rec_len)
    rec[0] = rec_len
    rec[1] = 0                       # extended attribute length
    rec[2:6] = struct.pack("<I", extent)
    rec[6:10] = struct.pack(">I", extent)
    rec[10:14] = struct.pack("<I", size)
    rec[14:18] = struct.pack(">I", size)
    rec[18:25] = ISO_DATE
    rec[25] = flags                  # file flags (bit0 = directory)
    rec[28:30] = struct.pack("<H", 1)  # volume sequence LE
    rec[30:32] = struct.pack(">H", 1)  # volume sequence BE
    rec[32] = len(name)
    rec[33:33 + len(name)] = name
    return bytes(rec)


def iso_pad_record_list(records):
    out = bytearray()
    for rec in records:
        out += rec
        if len(out) % 2:
            out += b"\x00"
    return bytes(out)


def eltorito_validation_entry():
    """32 字节 validation entry, 16bit 字和校验 (总和 ≡ 0 mod 0x10000)"""
    e = bytearray(32)
    e[0] = 0x01          # header id (EDK2 要求 0x01)
    e[1] = 0x00          # platform id: 0 = x86
    ident = b"EwokOS x86 boot"
    e[4:4 + len(ident)] = ident           # 24 字节标识区
    e[30:32] = b"\x55\xaa"
    total = sum(struct.unpack("<16H", bytes(e))) & 0xFFFF
    e[28:30] = struct.pack("<H", (0x10000 - total) & 0xFFFF)
    return bytes(e)


def eltorito_entry(boot_ind, media, load_seg, sys_type, sector_count, load_rba):
    """32 字节 initial/default 或 section entry"""
    assert 0 <= sector_count <= 0xFFFF
    e = bytearray(32)
    e[0] = boot_ind          # 0x88 = bootable
    e[1] = media             # 0x00 无仿真 / 0x04 硬盘仿真
    e[2:4] = struct.pack("<H", load_seg)
    e[4] = sys_type
    e[6:8] = struct.pack("<H", sector_count)
    e[8:12] = struct.pack("<I", load_rba)
    return bytes(e)


def eltorito_section_header(entries, ident):
    """32 字节 section header (0x91 = 最后一个 section)"""
    h = bytearray(32)
    h[0] = 0x91
    h[1] = 0xEF          # platform id: EFI
    h[2:4] = struct.pack("<H", entries)
    h[4:4 + len(ident)] = ident
    return bytes(h)


def iso_pvd(root_size, volume_size):
    pvd = bytearray(ISO_BLOCK)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    pvd[8:40] = b"LINUX".ljust(32)                       # system identifier
    pvd[40:72] = b"EWOKOS".ljust(32)                     # volume identifier
    pvd[80:84] = struct.pack("<I", volume_size)
    pvd[84:88] = struct.pack(">I", volume_size)
    pvd[120:122] = struct.pack("<H", 1)                  # volume set size
    pvd[124:126] = struct.pack("<H", 1)                  # volume seq
    pvd[128:130] = struct.pack("<H", ISO_BLOCK)          # logical block size
    pvd[140:144] = struct.pack("<I", PATH_TABLE_LBA)     # type-L path table
    pvd[148:152] = struct.pack(">I", PATH_TABLE_LBA + 1) # type-M path table
    pvd[156:190] = iso_dir_record(ROOT_DIR_LBA, root_size, 0x02, b"\x00")
    pvd[190:318] = b"EWOKOS".ljust(128)                  # volume set id
    pvd[446:574] = b"EwokOS mkboot.py".ljust(128)        # data preparer
    pvd[574:702] = b"EwokOS".ljust(128)                  # application
    pvd[813:830] = ISO_LONG_DATE                         # creation date
    pvd[830:847] = ISO_LONG_DATE                         # modification date
    pvd[847:864] = b"0" * 16 + b"\x00"                   # expiration (空)
    pvd[864:881] = ISO_LONG_DATE                         # effective date
    pvd[881] = 1                                         # file structure version
    return bytes(pvd)


def iso_boot_record_vd(catalog_lba):
    brvd = bytearray(ISO_BLOCK)
    brvd[0] = 0
    brvd[1:6] = b"CD001"
    brvd[6] = 1
    # 引导系统标识符按规范用 0 填充 (SeaBIOS 对偏移 1 做 strcmp 到 NUL,
    # 空格填充会 boot failed code 0005)
    brvd[7:30] = b"EL TORITO SPECIFICATION"
    brvd[71:75] = struct.pack("<I", catalog_lba)
    return bytes(brvd)


def iso_path_tables():
    """仅根目录一条记录: len, extattr, extent, parent, dirid ('\0')"""
    l_tbl = bytes(bytearray([
        1, 0,
        *struct.pack("<I", ROOT_DIR_LBA),
        *struct.pack("<H", 1),
        0,
        0,   # 补偶
    ]))
    m_tbl = bytes(bytearray([
        1, 0,
        *struct.pack(">I", ROOT_DIR_LBA),
        *struct.pack(">H", 1),
        0,
        0,   # 补偶
    ]))
    return l_tbl, m_tbl


def build_catalog(stage1_lba, esp_lba, esp_sectors, total_512):
    cat = bytearray()
    cat += eltorito_validation_entry()
    # BIOS default entry: no-emulation (Linux live ISO/ISOLINUX 同款)。
    # 真机 BIOS 对 hd-emulation 虚拟盘的几何/大小/扩展读支持各异
    # (ewokos.iso 真机黑屏的根因); no-emulation 下 BIOS 只装载 4x512B =
    # 2KB 的 cd_stage1 到 0x7C00, DL = El Torito CD 盘号, 之后 stage-1
    # 自主经 INT13h CD 扩展读完成全部装载。
    # no-emulation: BIOS 一次装载 loader(2KB) + 内核(~210KB) 到 0x7C00
    cat += eltorito_entry(0x88, 0x00, 0x07c0, 0, total_512, stage1_lba)
    cat += eltorito_section_header(1, b"UEFI")
    # EFI section entry: 无仿真, 指向 ESP FAT 镜像 (BOOTX64.EFI)
    cat += eltorito_entry(0x88, 0x00, 0, 0xEF, esp_sectors, esp_lba)
    assert len(cat) <= ISO_BLOCK
    return bytes(cat)


def build_iso(argv):
    """mkboot.py iso BOOT_BIN STAGE1_BIN KERNEL_IMG KERNEL_ELF ROOTFS_IMG ESP_IMG OUT"""
    if len(argv) != 8:
        raise SystemExit(
            "usage: mkboot.py iso BOOT_BIN STAGE1_BIN KERNEL_IMG "
            "KERNEL_ELF ROOTFS_IMG ESP_IMG OUT")

    boot = Path(argv[1]).read_bytes()
    stage1 = Path(argv[2]).read_bytes()
    kernel = Path(argv[3]).read_bytes()
    elf = Path(argv[4]).read_bytes()
    rootfs = Path(argv[5]).read_bytes()
    esp = Path(argv[6]).read_bytes()
    out_path = Path(argv[7])
    if len(boot) != 512:
        raise SystemExit(f"boot sector size mismatch: {len(boot)}")
    if len(stage1) > ISO_BLOCK:
        raise SystemExit(f"cd stage-1 too large: {len(stage1)} (> 2048)")

    # BIOS 路径 = El Torito no-emulation (Linux live ISO 同款):
    #   stage-1 (cd_stage1, 2KB) 由 BIOS 装载, 自主经 INT13h CD 扩展读
    #   加载紧随其后的 stage-2 与内核/内存盘。文件区不受 hd 仿真盘约束。
    stage1_lba = 24                    # 1 x 2048B 块
    kernel_lba = stage1_lba + 1        # KERNEL.IMG 紧随 loader      # KERNEL.IMG (扁平内核)
    elf_blocks = (len(elf) + ISO_BLOCK - 1) // ISO_BLOCK
    elf_lba = kernel_lba + (len(kernel) + ISO_BLOCK - 1) // ISO_BLOCK
    rootfs_lba = align_up(elf_lba + elf_blocks, ISO_BLOCK // SECTOR_SIZE)
    esp_lba = align_up(rootfs_lba + (len(rootfs) + ISO_BLOCK - 1) // ISO_BLOCK,
                       ISO_BLOCK // SECTOR_SIZE)
    esp_blocks = (len(esp) + ISO_BLOCK - 1) // ISO_BLOCK
    volume_size = esp_lba + esp_blocks
    # 内核在 512B 扇区空间的 LBA (文件区 KERNEL.IMG 拷贝, MBR/U盘路径用;
    # system area 字节 512 起的 isohybrid 拷贝被 PVD@16 截断, 不可引导)
    kernel_sector = kernel_lba * (ISO_BLOCK // SECTOR_SIZE)
    # stage1 回退装载槽: 内核 2KB LBA (真机 BIOS 截断 no-emulation 装载时,
    # cd_stage1 经 INT13h CD 扩展读自行装载 —— LBA 单位为 2KB 扇区)
    stage1 = bytearray(stage1)
    patch_slot(stage1, KCDL_MAGIC, kernel_lba, "kcdl_slot")

    # 根目录: '.', '..', KERNEL.ELF;1, KERNEL.IMG;1 (扁平内核, BIOS-CD
    # stage-2 加载), ROOTFS.IMG;1 (单扇区)
    root_dir = iso_pad_record_list([
        iso_dir_record(ROOT_DIR_LBA, ISO_BLOCK, 0x02, b"\x00"),
        iso_dir_record(ROOT_DIR_LBA, ISO_BLOCK, 0x02, b"\x01"),
        iso_dir_record(elf_lba, len(elf), 0x00, b"KERNEL.ELF;1"),
        iso_dir_record(kernel_lba, len(kernel), 0x00, b"KERNEL.IMG;1"),
        iso_dir_record(rootfs_lba, len(rootfs), 0x00, b"ROOTFS.IMG;1"),
    ])
    assert len(root_dir) <= ISO_BLOCK

    # EFI 入口 SectorCount 用 512B 虚拟扇区 (El Torito 规范/EDK2 ElTorito
    # 驱动按 SectorCount*512 计算分区大小; 传 2048 块数会把分区算成 1/4,
    # FAT 挂载失败, OVMF 掉 UEFI Shell);
    # BIOS hd 仿真入口保持 512B 虚拟扇区 (El Torito 规范/SeaBIOS 语义)
    esp_sectors = (len(esp) + SECTOR_SIZE - 1) // SECTOR_SIZE
    # no-emulation 装载量: RAM 0x7C00 起 stage-1 占一个 2048B 块 (文件未必
    # 满), 内核从 RAM 偏移 2048 (KERN_OFF) 开始 —— 按 2048 + 内核长度计,
    # 按文件长度算会漏装内核尾部 (cd_stage1.bin 仅 ~150B, 差 3 个扇区)
    total_512 = (ISO_BLOCK + len(kernel) + 511) // 512
    catalog = build_catalog(stage1_lba, esp_lba, esp_sectors, total_512)
    pvd = iso_pvd(len(root_dir), volume_size)

    iso = bytearray(volume_size * ISO_BLOCK)

    def put_bytes(offset, data):
        iso[offset:offset + len(data)] = data

    def put_block(lba, data):
        put_bytes(lba * ISO_BLOCK, data)

    # system area: MBR (446B 引导码 + p1 内核分区表项 + 0x55AA) + 内核裸镜像。
    # isohybrid 式: dd 到 U 盘可经 MBR/INT13h 引导 (El Torito CD 引导不再
    # 依赖 hd 仿真)。p1 指向文件区 KERNEL.IMG (字节 512 起的拷贝被 PVD
    # 截断, 仅供容量参考); kernel_start_lba 槽补丁为同一 LBA
    kernel_sectors = (len(kernel) + SECTOR_SIZE - 1) // SECTOR_SIZE
    mbr = bytearray(boot[:MBR_CODE_SIZE]
                    + build_partition_entry(kernel_sector, kernel_sectors,
                                            part_type=0x83, bootable=True,
                                            end_chs=(1023, 254, 63))
                    + b"\x00" * (64 - 16) + b"\x55\xaa")
    patch_kernel_lba(mbr, kernel_sector)
    put_bytes(0, mbr)
    put_bytes(SECTOR_SIZE, kernel)
    put_block(PVD_LBA, pvd)
    put_block(BRVD_LBA, iso_boot_record_vd(CATALOG_LBA))
    put_block(TERM_LBA, bytes([255]) + b"CD001" + bytes([1]))
    put_block(CATALOG_LBA, catalog)
    l_tbl, m_tbl = iso_path_tables()
    put_block(PATH_TABLE_LBA, l_tbl)
    put_block(PATH_TABLE_LBA + 1, m_tbl)
    put_block(ROOT_DIR_LBA, root_dir)
    put_block(stage1_lba, stage1)
    put_block(kernel_lba, kernel)
    put_block(elf_lba, elf)
    put_block(rootfs_lba, rootfs)
    put_block(esp_lba, esp)

    out_path.write_bytes(iso)
    print(f"mkboot: iso {out_path} volume={volume_size} blocks "
          f"(stage1@{stage1_lba} kernel@{kernel_lba} "
          f"elf@{elf_lba} rootfs@{rootfs_lba} esp@{esp_lba})")


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: mkboot.py <cfg|image|hdd|iso> ...")

    mode = sys.argv[1]
    if mode == "cfg":
        gen_cfg(sys.argv[1:])
    elif mode == "image":
        build_image(sys.argv[1:])
    elif mode == "hdd":
        build_hdd(sys.argv[1:])
    elif mode == "iso":
        build_iso(sys.argv[1:])
    else:
        raise SystemExit(f"unknown mode: {mode}")


if __name__ == "__main__":
    main()
