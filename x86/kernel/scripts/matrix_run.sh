#!/usr/bin/env bash
# ewokos x86 兼容性矩阵: BIOS/UEFI × 机型 × 存储驱动配置
# 用法: ./matrix_run.sh [1..7]  (可只跑某一项)
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
KDIR="$DIR/.."
OVMF_SRC="${OVMF:-/opt/homebrew/share/qemu/edk2-x86_64-code.fd}"
[ -f "$KDIR/boot/uefi/OVMF.fd" ] || cp "$OVMF_SRC" "$KDIR/boot/uefi/OVMF.fd"

UEFI="-drive if=pflash,format=raw,file=boot/uefi/OVMF.fd,readonly=on"
USB="-usb -device usb-kbd,port=1,usb_version=1"
SER="-no-reboot -no-shutdown"

qemu_args() {
  case "$1" in
    1) echo "-accel tcg -M pc -m 512 -display none $SER -serial mon:stdio -boot c -drive file=kernel.hdd,format=raw,if=ide,index=0,media=disk $USB" ;;
    2) echo "-accel tcg -M pc -m 512 -display none $SER -serial mon:stdio $UEFI -boot c -drive file=kernel.hdd,format=raw,if=ide,index=0,media=disk $USB" ;;
    3) echo "-accel tcg -M pc -m 512 -display none $SER -serial mon:stdio -boot c -drive file=kernel.hdd,format=raw,if=ide,index=2,media=disk $USB" ;;
    4) echo "-accel tcg -M pc -m 512 -display none $SER -serial mon:stdio $UEFI -boot c -drive file=kernel.hdd,format=raw,if=ide,index=2,media=disk $USB" ;;
    5) echo "-accel tcg -M pc -m 512 -display none $SER -serial mon:stdio -boot c -drive file=kernel.hdd,format=raw,if=ide,index=1,media=disk $USB" ;;
    6) echo "-accel tcg -M q35 -m 512 -display none $SER -serial mon:stdio -drive file=kernel.hdd,format=raw,if=none,id=hd0 -device ich9-ahci,id=ahci0 -device ide-hd,drive=hd0,bus=ahci0.0 $USB" ;;
    7) echo "-accel tcg -M q35 -m 512 -display none $SER -serial mon:stdio $UEFI -drive file=kernel.hdd,format=raw,if=none,id=hd0 -device ich9-ahci,id=ahci0 -device ide-hd,drive=hd0,bus=ahci0.0 $USB" ;;
    *) return 1 ;;
  esac
}

name_of() {
  case "$1" in
    1) echo "pc-bios-ide0" ;;
    2) echo "pc-uefi-ide0" ;;
    3) echo "pc-bios-ide2" ;;
    4) echo "pc-uefi-ide2" ;;
    5) echo "pc-bios-ide1" ;;
    6) echo "q35-bios-ahci" ;;
    7) echo "q35-uefi-ahci" ;;
  esac
}

ONLY="${1:-}"
PASS=0; FAIL=0; FAILED=""
for i in 1 2 3 4 5 6 7; do
  [ -n "$ONLY" ] && [ "$i" != "$ONLY" ] && continue
  name=$(name_of "$i")
  args=$(qemu_args "$i")
  echo "=== [$i] $name ==="
  pkill -f qemu-system-x86_64 2>/dev/null; sleep 1
  TMO=120
  case "$i" in 6|7) TMO=600 ;; esac
  ( export QEMU_ARGS="$args" TIMEOUT="$TMO"; expect "$DIR/matrix_boot.exp" "$name" > "/tmp/matrix_${name}_out.log" 2>&1 )
  result=$(grep -oE "RESULT $name: (PASS|FAIL)[^\r]*" "/tmp/matrix_${name}_out.log" 2>/dev/null | tail -1 | sed "s/RESULT $name: //")
  echo "  → $result"
  case "$result" in
    PASS*) PASS=$((PASS+1)) ;;
    *) FAIL=$((FAIL+1)); FAILED="$FAILED [$i:$name]" ;;
  esac
done
pkill -f qemu-system-x86_64 2>/dev/null
echo
echo "=== 矩阵结果: PASS=$PASS FAIL=$FAIL ==="
[ -n "$FAILED" ] && echo "失败项:$FAILED"
exit 0
