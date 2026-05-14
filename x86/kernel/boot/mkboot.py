#!/usr/bin/env python3
from pathlib import Path
import re
import subprocess
import sys


FLOPPY_144_SIZE = 1474560


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
    if len(image) > FLOPPY_144_SIZE:
        raise SystemExit(f"boot image too large for 1.44MB floppy: {len(image)} bytes")
    padding = FLOPPY_144_SIZE - len(image)
    out_path.write_bytes(image + (b"\0" * padding))


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: mkboot.py <cfg|image> ...")

    mode = sys.argv[1]
    if mode == "cfg":
        gen_cfg(sys.argv[1:])
    elif mode == "image":
        build_image(sys.argv[1:])
    else:
        raise SystemExit(f"unknown mode: {mode}")


if __name__ == "__main__":
    main()
