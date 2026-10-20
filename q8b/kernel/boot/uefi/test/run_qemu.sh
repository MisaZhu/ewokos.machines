#!/bin/sh
# QEMU smoke test for the Q8B UEFI loader.
#
# This does NOT run the Q8B kernel (it needs SC8280XP's GENI UART/GICv3).
# It builds BOOTAA64.EFI with Q8B_QEMU_TEST, places it together with the
# mock kernel and a dummy rootfs on a FAT ESP, boots QEMU virt with the
# EDK2 AArch64 firmware and verifies:
#   * SimpleFileSystem loading of KERNEL.BIN / ROOTFS.IMG,
#   * bootinfo page construction,
#   * ExitBootServices and the jump to 0x40080000.
#
# Usage: ./run_qemu.sh [timeout-seconds]
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
UEFI=$(cd "$HERE/.." && pwd)
KERNEL=$(cd "$UEFI/../.." && pwd)
OUT="$HERE/out"
TC=${TC:-/Users/tony/toolchain/arm-gnu-toolchain-14.3.rel1-darwin-arm64-aarch64-none-elf/bin}
CC="$TC/aarch64-none-elf-gcc"
LD="$TC/aarch64-none-elf-ld"
OBJCOPY="$TC/aarch64-none-elf-objcopy"
FW=${FW:-/opt/homebrew/share/qemu/edk2-aarch64-code.fd}
TIMEOUT=${1:-40}

mkdir -p "$OUT"

echo "== build test loader =="
"$CC" -DQ8B_QEMU_TEST -ffreestanding -fno-stack-protector -fno-stack-check \
    -fno-asynchronous-unwind-tables -fno-unwind-tables -fPIE \
    -fvisibility=hidden -mgeneral-regs-only -mstrict-align -O2 -Wall -Wextra \
    -I"$UEFI" -c "$UEFI/main.c" -o "$OUT/main.o"
"$LD" "$OUT/main.o" -T "$UEFI/linker.ld" -e efi_main -o "$OUT/boot.elf"
python3 "$UEFI/mkpefi.py" "$OUT/boot.elf" "$OUT/BOOTAA64.EFI" 10

echo "== build mock payload =="
"$CC" -c "$HERE/mock_kernel.S" -o "$OUT/mock.o"
"$LD" "$OUT/mock.o" -Ttext=0x80080000 -e _start -o "$OUT/mock.elf"
"$OBJCOPY" -O binary "$OUT/mock.elf" "$OUT/KERNEL.BIN"

# Dummy rootfs: the loader only needs a byte range to hand to the kernel.
dd if=/dev/zero of="$OUT/ROOTFS.IMG" bs=1m count=1 2>/dev/null

echo "== build FAT ESP =="
rm -f "$OUT/esp.img"
dd if=/dev/zero of="$OUT/esp.img" bs=1m count=64 2>/dev/null
mformat -i "$OUT/esp.img" -F ::
mmd -i "$OUT/esp.img" ::/EFI ::/EFI/BOOT
mcopy -i "$OUT/esp.img" "$OUT/BOOTAA64.EFI" ::/EFI/BOOT/BOOTAA64.EFI
mcopy -i "$OUT/esp.img" "$OUT/KERNEL.BIN" ::/KERNEL.BIN
mcopy -i "$OUT/esp.img" "$OUT/ROOTFS.IMG" ::/ROOTFS.IMG

echo "== run QEMU (${TIMEOUT}s) =="
LOG="$OUT/qemu.log"
qemu-system-aarch64 \
    -M virt -cpu max -m 2G -nographic -no-reboot \
    -bios "$FW" \
    -drive file="$OUT/esp.img",format=raw,if=none,id=esp \
    -device virtio-blk-device,drive=esp \
    >"$LOG" 2>&1 &
QPID=$!
i=0
while [ "$i" -lt "$TIMEOUT" ]; do
    if ! kill -0 "$QPID" 2>/dev/null; then
        break
    fi
    sleep 1
    i=$((i + 1))
done
kill "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true
cat "$LOG"

echo "== result =="
if grep -q "MOCK KERNEL OK" "$LOG"; then
    echo "PASS: UEFI loader reached the payload"
    exit 0
fi
echo "FAIL: payload banner not seen (see $LOG)"
exit 1
