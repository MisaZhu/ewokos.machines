#!/usr/bin/env bash
# ewokos x86 外设测试一键运行: USB / SATA / NVMe
#   USB : pc 机型 + UHCI(usb-kbd/usb-mouse/usb-storage FAT32) → /mnt/udisk0 自动挂载
#   SATA: q35 + ich9-ahci + atafsd → /mnt/sata (ext3 标记文件)
#   NVMe: nvme 设备 + nvmefsd → /mnt/nvme (TCG 下已知首命令完成延迟, KVM 预期通过)
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
KDIR="$DIR/.."   # machines/x86/kernel
OVMF_SRC="${OVMF:-/opt/homebrew/share/qemu/edk2-x86_64-code.fd}"

run_uefi() {  # $1=log
  local f="/tmp/$1"
  rm -f "$f"
  cp "$OVMF_SRC" "$KDIR/boot/uefi/OVMF.fd" 2>/dev/null
  (cd "$KDIR" && qemu-system-x86_64 -accel tcg -M pc -m 512 -display none \
     -no-reboot -no-shutdown \
     -serial file:"$f" \
     -drive if=pflash,format=raw,file=boot/uefi/OVMF.fd,readonly=on \
     -boot c -drive file=kernel.hdd,format=raw,if=ide,index=0,media=disk \
     -device ahci,id=ahci0 -drive id=dsata,file=sata_test.img,format=raw,if=none \
     -device ide-hd,drive=dsata,bus=ahci0.0 \
     -device nvme,id=nv0,serial=ewok0 -device nvme-ns,bus=nv0,drive=dnvme \
     -drive id=dnvme,file=nvme_test.img,format=raw,if=none \
     -usb -device usb-kbd,port=1,usb_version=1 -device usb-mouse,port=2,usb_version=1 \
     -drive id=ustick,file=usb_test.img,format=raw,if=none
     -device usb-storage,drive=ustick \
     >/dev/null 2>&1 & echo $! > /tmp/q.pid)
  sleep "$2"
  kill $(cat /tmp/q.pid) 2>/dev/null
  echo "$f"
}

echo "=== USB / SATA / NVMe 外设测试 (pc + UEFI) ==="
LOG=$(run_uefi uefi_periph.log 75)

check() {  # $1=名称 $2=期望串
  if grep -q "$2" "$LOG" 2>/dev/null; then
    echo "PASS: $1"
  else
    echo "FAIL: $1"
  fi
}
check "USB: usbhostd 枚举到设备"     "usbhostd:"
check "USB: /dev/hid0 就绪"          "/dev/hid0"
check "USB: MSC 自动挂载 /mnt/udisk0" "udisk"
check "USB: 标记文件内容回读"         "hello from usb stick"
check "SATA: atafd 探测到端口"        "ahci: .*ready\|identify rc=0\|port 0 started"
check "SATA: /mnt/sata 挂载+标记"     "hello from sata"
check "NVMe: 探测/使能完成"           "nvme ready"
check "NVMe: /mnt/nvme 挂载(TCG 预期 FAIL)" "hello from nvme"
echo
echo "串口日志: $LOG  |  KVM 宿主复测: scripts/run-arch-kvm.sh 后同步骤"
