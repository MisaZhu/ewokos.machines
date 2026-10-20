#!/bin/sh
# QEMU smoke test for the Q8B *kernel* boot path.
#
# Builds the loader with Q8B_QEMU_TEST (same code as the board, QEMU's DRAM
# base/PL011/GICv2), puts the real kernel_qemu.bin and the ext3 rootfs image
# on a FAT ESP and boots EDK2.  This validates the RAM_BASE-aware linker
# script, early page tables, bootinfo parsing, ext3/loadinit and the jump to
# user space.  The QCOM GENI/GICv3 drivers are not exercised here.
#
# Usage: ./run_kernel_qemu.sh [timeout-seconds]
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
UEFI=$(cd "$HERE/.." && pwd)
KERNDIR=$(cd "$UEFI/../.." && pwd)
Q8BDIR=$(cd "$KERNDIR/.." && pwd)
OUT="$HERE/out-kernel"
TC=${TC:-/Users/tony/toolchain/arm-gnu-toolchain-14.3.rel1-darwin-arm64-aarch64-none-elf/bin}
CC="$TC/aarch64-none-elf-gcc"
LD="$TC/aarch64-none-elf-ld"
FW=${FW:-/opt/homebrew/share/qemu/edk2-aarch64-code.fd}
TIMEOUT=${1:-90}
KERNEL_BIN=${KERNEL_BIN:-$KERNDIR/kernel_qemu.bin}
ROOTFS_IMG=${ROOTFS_IMG:-$Q8BDIR/root_aarch64.img}
ESP_MB=${ESP_MB:-256}

if [ ! -f "$KERNEL_BIN" ]; then
    echo "missing $KERNEL_BIN -- run 'make TEST_QEMU=1' in $KERNDIR"
    exit 1
fi
if [ ! -f "$ROOTFS_IMG" ]; then
    echo "missing $ROOTFS_IMG -- run 'make gui && make sd' in $Q8BDIR/system"
    exit 1
fi

mkdir -p "$OUT"

echo "== build test loader =="
"$CC" -DQ8B_QEMU_TEST -ffreestanding -fno-stack-protector -fno-stack-check \
    -fno-asynchronous-unwind-tables -fno-unwind-tables -fPIE \
    -fvisibility=hidden -mgeneral-regs-only -mstrict-align -O2 -Wall -Wextra \
    -I"$UEFI" -c "$UEFI/main.c" -o "$OUT/main.o"
"$LD" "$OUT/main.o" -T "$UEFI/linker.ld" -e efi_main -o "$OUT/boot.elf"
python3 "$UEFI/mkpefi.py" "$OUT/boot.elf" "$OUT/BOOTAA64.EFI" 10

echo "== build FAT ESP (${ESP_MB}MB) =="
rm -f "$OUT/esp.img"
dd if=/dev/zero of="$OUT/esp.img" bs=1m count="$ESP_MB" 2>/dev/null
mformat -i "$OUT/esp.img" -F ::
mmd -i "$OUT/esp.img" ::/EFI ::/EFI/BOOT
mcopy -i "$OUT/esp.img" "$OUT/BOOTAA64.EFI" ::/EFI/BOOT/BOOTAA64.EFI
mcopy -i "$OUT/esp.img" "$KERNEL_BIN" ::/KERNEL.BIN
mcopy -i "$OUT/esp.img" "$ROOTFS_IMG" ::/ROOTFS.IMG

echo "== run QEMU (${TIMEOUT}s, kernel $(wc -c < "$KERNEL_BIN") bytes, rootfs $(wc -c < "$ROOTFS_IMG") bytes) =="
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
if grep -q "init: /sbin/sdfsd" "$LOG" && grep -q "\[ok\]" "$LOG"; then
    echo "PASS: kernel booted, loaded init, core/vfsd/sdfsd started"
    exit 0
fi
echo "FAIL: kernel did not reach loadinit (see $LOG)"
exit 1
