#!/usr/bin/env bash
# ewokos x86 — Arch Linux x64 + KVM 运行脚本
#
# 磁盘镜像 kernel.hdd 与构建宿主无关(macOS/Linux make 产物通用), 拷到本机后:
#   ./run-arch-kvm.sh [磁盘路径] [uefi|bios|q35|q35-uefi]
#
# 依赖(Arch):
#   sudo pacman -S --needed qemu-full edk2-ovmf
#   # 仅 qemu-desktop 也可(qemu-full 含全体系模拟)
#
# KVM 前提:
#   1. BIOS/UEFI 设置中开启 VT-x(Intel) 或 AMD-V(SVM)
#   2. 宿主内核模块: modprobe kvm_intel   (Intel)
#                     modprobe kvm_amd    (AMD)
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
HDD="${1:-$DIR/../kernel.hdd}"
MODE="${2:-uefi}"   # uefi | bios | q35 | q35-uefi

[ -f "$HDD" ] || { echo "磁盘不存在: $HDD (先在构建机 make kernel.hdd)"; exit 1; }
if [ ! -w /dev/kvm ]; then
  echo "KVM 不可用: /dev/kvm 不存在或无权限"
  echo "  - BIOS 里开启 VT-x/AMD-V"
  echo "  - sudo modprobe kvm_intel   # Intel"
  echo "  - sudo modprobe kvm_amd     # AMD"
  echo "  - 将自己加入 kvm 组: usermod -aG kvm \$USER"
  exit 1
fi

OVMF=""
for f in /usr/share/edk2/x64/OVMF_CODE.4M.fd \
         /usr/share/OVMF/x64/OVMF_CODE.fd \
         /usr/share/ovmf/x64/OVMF_CODE.fd; do
  [ -f "$f" ] && OVMF="$f" && break
done

BASE=(-enable-kvm -cpu host -M pc -m 1024
      -serial mon:stdio -no-reboot -no-shutdown
      -drive file="$HDD",format=raw,if=ide,index=0,media=disk
      -usb -device usb-kbd,port=1,usb_version=1)

case "$MODE" in
  bios)
    exec qemu-system-x86_64 "${BASE[@]}" -boot c
    ;;
  uefi)
    [ -n "$OVMF" ] || { echo "未找到 OVMF 固件(pacman -S edk2-ovmf)"; exit 1; }
    exec qemu-system-x86_64 "${BASE[@]}" -boot c \
      -drive if=pflash,format=raw,readonly=on,file="$OVMF"
    ;;
  q35|q35-uefi)
    # q35: 无 legacy IDE, 根盘走 AHCI(内核 sd.c 自动回退)
    Q=(qemu-system-x86_64 -enable-kvm -cpu host -M q35 -m 1024
       -serial mon:stdio -no-reboot -no-shutdown
       -drive file="$HDD",format=raw,if=none,id=hd0
       -device ich9-ahci,id=ahci0 -device ide-hd,drive=hd0,bus=ahci0.0
       -usb)
    [ "$MODE" = "q35-uefi" ] && {
      [ -n "$OVMF" ] || { echo "未找到 OVMF 固件"; exit 1; }
      Q+=(-drive if=pflash,format=raw,readonly=on,file="$OVMF")
    }
    exec "${Q[@]}"
    ;;
  *)
    echo "用法: $0 [kernel.hdd] [uefi|bios|q35|q35-uefi]"
    exit 1
    ;;
esac
