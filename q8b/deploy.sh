#!/bin/sh
# Deploy the Q8B payload to the PXE/TFTP tree and build a bootable USB image.
#
#   ./deploy.sh [tftp-q8b-dir]
#
# Default TFTP destination: ../../../q8b/boot/tftp/q8b
# (the q8b/ working tree next to ewokos/).
#
# Outputs:
#   <tftp>/BOOTAA64.EFI, KERNEL.BIN   -- PXE payload; rootfs is on SD
#   build/q8b-esp.img                              -- FAT32 ESP image for USB
#
# The ESP image can be written to a USB stick with:
#   sudo dd if=build/q8b-esp.img of=/dev/rdiskN bs=1m
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
DEST=${1:-$(cd "$HERE/../../.." && pwd)/q8b/boot/tftp/q8b}

KERNEL="$HERE/kernel/kernel.bin"
STUB="$HERE/kernel/boot/uefi/BOOTAA64.EFI"
ROOTFS="$HERE/root_aarch64.img"
SD_ROOTFS=${SD_ROOTFS:-1}

for f in "$KERNEL" "$STUB"; do
    if [ ! -f "$f" ]; then
        echo "missing $f -- run 'make payload' first"
        exit 1
    fi
done
if [ "$SD_ROOTFS" != 1 ] && [ ! -f "$ROOTFS" ]; then
    echo "missing $ROOTFS -- run 'make payload' first"
    exit 1
fi

echo "== copy to $DEST =="
mkdir -p "$DEST"
cp "$STUB" "$DEST/BOOTAA64.EFI"
cp "$KERNEL" "$DEST/KERNEL.BIN"
if [ "$SD_ROOTFS" = 1 ]; then
    rm -f "$DEST/ROOTFS.IMG"
else
    cp "$ROOTFS" "$DEST/ROOTFS.IMG"
fi
ls -l "$DEST"

echo "== build MBR + FAT32 ESP image (build/q8b-esp.img) =="
BUILD="$HERE/build"
mkdir -p "$BUILD"
ESP="$BUILD/q8b-esp.img"
rm -f "$ESP"

# 128MB rootfs + kernel + FAT overhead, rounded up to a comfortable 384MB.
# Use a real MBR partition table with one FAT32 ESP starting at 1MiB: many
# UEFI firmware implementations only enumerate a USB stick through its
# partition table, not a "superfloppy" (whole-disk FAT) image.
SIZE_MB=${ESP_MB:-384}
ESP_OFF=1048576
dd if=/dev/zero of="$ESP" bs=1m count="$SIZE_MB" 2>/dev/null
python3 - "$ESP" "$ESP_OFF" <<'PY'
import os, struct, sys
path, off = sys.argv[1], int(sys.argv[2])
size = os.path.getsize(path)
start_lba = off // 512
total_sectors = size // 512 - start_lba
with open(path, "r+b") as f:
    mbr = bytearray(512)
    # status, CHS start, type=0x0C (FAT32 LBA), CHS end, LBA start, sectors
    mbr[0x1BE:0x1BE + 16] = struct.pack(
        "<B3sB3sII", 0x80, b"\xfe\xff\xff", 0x0C, b"\xfe\xff\xff",
        start_lba, total_sectors)
    mbr[510:512] = b"\x55\xaa"
    f.seek(0)
    f.write(mbr)
PY
mformat -i "$ESP@@$ESP_OFF" -F ::
mmd -i "$ESP@@$ESP_OFF" ::/EFI ::/EFI/BOOT
mcopy -i "$ESP@@$ESP_OFF" "$STUB" ::/EFI/BOOT/BOOTAA64.EFI
mcopy -i "$ESP@@$ESP_OFF" "$KERNEL" ::/KERNEL.BIN
if [ "$SD_ROOTFS" = 1 ]; then
    echo "SD_ROOTFS=1: ESP contains loader+kernel; rootfs stays on the SD ext3 partition"
else
    mcopy -i "$ESP@@$ESP_OFF" "$ROOTFS" ::/ROOTFS.IMG
fi
ls -l "$ESP"

cat <<'EOF'

== next steps ==
USB: write build/q8b-esp.img to a USB stick, insert it into the Q8B, press
     F12 and choose the USB device (or Boot Maintenance Manager -> Boot From
     File -> \EFI\BOOT\BOOTAA64.EFI).

PXE: copy BOOTAA64.EFI and KERNEL.BIN to the TFTP root served by
     q8b/boot/start.sh and point the DHCP bootfile at q8b/BOOTAA64.EFI.
     With SD_ROOTFS=1 the loader reads only \q8b\KERNEL.BIN through
     EFI_LOAD_FILE (TFTP); rootfs is read from the SD ext3 partition.
     GRUB chainload may work, but a direct PXE NBP is the reliable path
     because it keeps the firmware's network LoadFile handle.

Serial: 40-pin Pin6=GND, Pin8=TXD, Pin10=RXD, 115200 8N1 (do NOT connect VCC).
BIOS:   F2 -> Debugging Settings -> Synchronous Debug UART in UEFI = Enabled
        to see the UEFI-stage log as well.
EOF
