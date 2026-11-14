# 8gen3 (TW220 / Radxa Dragon Q8B, SC8280XP)

高通 SC8280XP (8cx Gen3) 板卡移植:Radxa Dragon Q8B 主板 + TW220 面板
(无有线 UART17 调试口、无物理 SD 卡槽,M.2 M-key 在 pcie2a/pcie3b)。
自举通道两条,互相独立:

- **PXE 网络启动**:UEFI 里 `Network` 设备最前,从 TFTP 拉
  `q8b/BOOTAA64.EFI` → KERNEL.BIN + INITRAMFS.IMG;rootfs 由 init 按策略
  挂 NVMe(p3 ext3)。日常调试主通道,固件/内核迭代不碰磁盘。
- **SSD grub 链**(盘上系统自带):UEFI NVMe → grub(ESP p1)→ 菜单
  "Ewokos" chainload **p2 BOOT 分区**的 `EFI/EWOKOS/BOOTAA64.EFI` →
  `\q8b\KERNEL.BIN` + `\q8b\INITRAMFS.IMG`(loader 根路径探测约定)→
  p3 ext3 rootfs。脱离网络做最终验证用。

盘上分区(p3 为 EwokOS rootfs 目标):

| 分区 | 大小 | 文件系统 | 用途 |
|---|---|---|---|
| p1 | 256M | vfat `SYSTEM` | ESP(grub/tw220 链,**勿动 BootOrder**) |
| p2 | 2G | vfat `BOOT`(B007-2026) | EwokOS 载荷 + 盘上 linux 内核 |
| p3 | 8G | ext3 | **EwokOS rootfs**(PARTUUID fb883841-…) |
| p4 | 220G | ext4 `TW220ROOT` | 盘上 Linux(Ubuntu 26.04,管理通道) |
| p5 | 8G | vfat `RECOVERY` | 恢复镜像 |

## 目录结构

```text
3rd/        第三方驱动(matebook himaxd 触摸等)
docs/       板级调试文档(wifi/bt/typec/npu/spi/usb-pcie 等)
efi/        UEFI loader(main.c)+ 载荷打包;
            产物 BOOTAA64.EFI / kernel.bin / initramfs.img,
            build/initramfs/ 为 ext3 打包暂存;部署时 PXE 服务根目录即本目录
firmware/   adreno(Qualcomm 签名 zap/GMU/SQE)、ath11k(nfa765 WLAN)、
            npu、qca(BT)
include/    平台头文件(q8b_platform.h 启动握手布局、q8b_adreno.h GPU 物理地址图)
kernel/     EwokOS 内核(bsp/aarch64/8gen3),产物 kernel/kernel.bin
skills/     q8b-kernel-test(复位/PXE/串口标准测试流程,先读)、q8b-tw220-spi-bringup
system/     make.inc(HW=8gen3,ARCH=aarch64)+ Makefile;
            drivers/8gen3(pcied/g2dd/wlan/usbhostd/net/uartd/powerd…)、
            libs/bsp(libbsp:nvme/pcie/dwc3/xhci/sdcc2/eth/spi…+ adreno)、
            etc/basic|gui|xwin(各阶段 profile 与 kernel.conf)、
            build_aarch64/(final 暂存,efi 打包与 p2/p3 烧写从这里取)
test/       USB/WLAN 主机侧测试工具
```

## 编译步骤

工具链:`aarch64-none-elf-gcc`(PATH 内)。三个产物各自独立:

```bash
make -C machines/qcom/8gen3/kernel     # 内核 -> kernel/kernel.bin
make -C machines/qcom/8gen3/system x   # 完整系统(xwin 桌面;basic=控制台版)
                                       #   构建 -> build_aarch64/8gen3/rootfs,
                                       #   final 拷贝 -> system/build_aarch64/
make -C machines/qcom/8gen3/efi        # 打包 -> efi/{BOOTAA64.EFI,kernel.bin,initramfs.img}
                                       #   initramfs 只打包 basic 组件(8M ext3)
```

system 目标递进:`basic ⊂ network ⊂ gui ⊂ x`(x 默认,含桌面与全部驱动)。
改 `system/basic` 下游(sdfsd/init 等)用 `make basic` 即可,但注意
`etc/xwin/init.rd` 只有 `make x` 会安装到暂存树——改了 profile 必须跑
`make x`,否则 p2/p3 拿到的是旧 profile(踩过:basic profile 的
`session -r` 在无 sessiond 时无限重启)。

efi/Makefile 常用开关:`INITRAMFS_MB`(镜像兆数)、`SELF_ROOT=1`(把
kernel.conf 的 rootfs=auto 改写成 self,维护模式)、`KERNEL_ONLY=1`
(只装内核,debug)、`FORCE_UART17/NO_SERIAL/MTFTP_FIRST/TEST_QEMU`。
`efi build/initramfs/` 是打包暂存,可用 `e2cp`/`debugfs` 抽查内容。

## 更新步骤

### A. PXE/TFTP(日常)

```bash
# 向 PXE 服务查询当前投放目录($TFTP_ROOT,目录可配置,不要写死)
cp efi/BOOTAA64.EFI  "$TFTP_ROOT/q8b/BOOTAA64.EFI"
cp efi/kernel.bin    "$TFTP_ROOT/q8b/KERNEL.BIN"
cp efi/initramfs.img "$TFTP_ROOT/q8b/INITRAMFS.IMG"
# 远程复位板子,串口抓 boot log 判定(复位/串口工具见 skills/q8b-kernel-test)
```

halt-kernel 隔离、KERNEL_ENTRY.BIN 保存/恢复等手法见
`skills/q8b-kernel-test/SKILL.md`。

### B. SSD(grub 链)更新

需要板子在盘上 Linux(其自身管理通道;从 EwokOS 切回:停掉 PXE 服务
再复位,板子自动落盘引导;或 F12 菜单手选——串口发 F12 键序列固件
不认,必须面板 USB 键盘)。下述命令在板子上执行,载荷先传到板子。

```bash
# 载荷已传入板子 /tmp(scp)
mount /dev/nvme0n1p1 /mnt/esp          # ESP,只读检查用
mount /dev/nvme0n1p2 /mnt/boot
cp /tmp/BOOTAA64.EFI   /mnt/boot/EFI/EWOKOS/BOOTAA64.EFI
cp /tmp/kernel.bin     /mnt/boot/q8b/KERNEL.BIN
cp /tmp/initramfs.img  /mnt/boot/q8b/INITRAMFS.IMG
cd /mnt/boot && sha256sum linux/Image linux/sc8280xp-ntmer-tw220.dtb \
    linux/initrd.img q8b/KERNEL.BIN q8b/INITRAMFS.IMG \
    EFI/EWOKOS/BOOTAA64.EFI > sha256sums.txt && sha256sum -c sha256sums.txt
sync; umount /mnt/boot; umount /mnt/esp
```

之后重启,grub 菜单选 "Ewokos" 即全 SSD 链路(loader 探测
`\q8b\KERNEL.BIN`/`\q8b\INITRAMFS.IMG`,均为 p2 根相对路径)。

**p3 rootfs 重建**(格式必须 `-I 128`,ext3 库不支持 256-byte inode,
症状:野块号读循环、journal inode #8 写坏):

```bash
umount /mnt/ewokos
mkfs.ext3 -b 4096 -I 128 -F /dev/nvme0n1p3
mount /dev/nvme0n1p3 /mnt/ewokos
rm -rf /mnt/ewokos/*; tar -xf rootfs.tar -C /mnt/ewokos   # tar 自 system/build_aarch64
chown -R root:root /mnt/ewokos; sync; umount /mnt/ewokos  # 注意先 cd 出挂载点再 umount
```

### C. 运行中热更新

EwokOS 起来后(ssh root@<板子IP>,或盘上 Linux 改 p3):

```bash
scp -O -l 50 <binary> root@<板子IP>:/drivers/8gen3/<name>   # 限速较稳
ssh root@<板子IP> /bin/md5 /drivers/8gen3/<name>            # 与本地比对
```

## 纪律与已知坑

- **UEFI BootOrder / Boot#### 条目一律不动**(efibootmgr -c 会插队改
  默认引导)。测试 EwokOS 走 grub 现成 "Ewokos" 菜单项或 PXE,别注册
  新启动项。
- p3 只能用 `-I 128`;EwokOS ext3 提交依赖 NVMe FLUSH(sdfsd 已接),
  未净复位后 fs 应保持 clean,若 Linux 侧报 not clean 先 e2fsck。
- PXE 偶发 DHCP 失败会静默回退:日志见 `[uefi] boot source=unknown`、
  无 `>>Start PXE over IPv4` 时直接再 reboot 一次。
- rootfs 策略:`kernel.conf` 的 `rootfs = nvme|auto|self|sd|usb`;nvme
  依赖 pcied 先发布 /dev/pcie0 快照(init 流程已保证);AUTO 顺序
  self → sd → usb → nvme → net。
- TW220 本板 wcn 固件加载 `wcn: failed stage=IPCR` 循环,待排查
  (与 rootfs 通道无关;面板桌面不受影响)。
- 串口救援台在 boot RAMFS profile(uartd/sessiond/session),穿越
  switchroot,登录 root/root;完整测试流程看
  `skills/q8b-kernel-test/SKILL.md`。
