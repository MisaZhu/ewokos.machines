# EwokOS x86 移植 (BIOS + UEFI)

本目标支持 **BIOS 与 UEFI 双固件路径**启动, 同一块磁盘镜像两种方式均可引导;
除传统 `-M pc`(legacy IDE) 外, 也支持 **`-M q35`**(无 legacy IDE, 根盘经
AHCI DMA 读取)。

## 启动流程

```
BIOS(SeaBIOS) 路径                          UEFI(OVMF) 路径
─────────────────                           ─────────────────
MBR 引导扇区 boot/bios_boot.S               \EFI\BOOT\BOOTX64.EFI (boot/uefi/)
  INT 13h 逐段读内核裸镜像                     EFI SimpleFileSystem 读 \KERNEL.ELF
  复制到物理 0x100000                          按 p_paddr 放置(0x100000 被固件
  校验入口 MAGIC                               占用时先暂存, ExitBootServices
  跳入 arch/x64/boot.S __entry                 后再拷回 0x100000, 同 Linux EFI stub)
        │                                     收集 EFI 内存映射 + GOP framebuffer
        │                                       → bootinfo (rdi 传入)
        │                                     跳 boot.S 的 uefi_entry (64 位)
        ▼                                          │
   长模式初始化(boot_pml4: 恒等 0-1GB + 0x80000000→0x100000 2MB)
        │   UEFI 路径把 bootinfo 指针存入 x86_uefi_bootinfo
        ▼
   x86_runtime_init → bsp/start.c → _kernel_entry_c
        │
   hw_info: UEFI 引导时解析 bootinfo(EFI 内存图 → 内存大小, GOP → fb),
            并做 PCI 探测缓存 AHCI ABAR(供内核 AHCI 引导路径)
        ▼
   sd_init: legacy ATA PIO(0x1F0, pc 机型)优先, 失败回退内核 AHCI(q35/现代 x86)
        ▼
   loadinit 读 /sbin/init → init 起 core/vfsd/sdfsd → init.rd 起各驱动 → login
```

两种固件共用同一份 `kernel.elf` 与同一块磁盘; UEFI stub 通过内核 ELF 的
`.symtab` 定位 `uefi_header/uefi_entry` 符号(boot.S 中保留 4 深队列无关,
见 `boot/uefi/main.c`)。

## 构建 & 运行

```bash
# 内核 + 磁盘(含 ESP/根文件系统)
cd machines/x86/kernel && make

# 用户态(rootfs)
cd machines/x86/system && make basic && make sd

cd machines/x86/kernel
make run            # BIOS (SeaBIOS), -M pc, legacy IDE
make run-uefi       # UEFI (OVMF),  -M pc, legacy IDE
make run-q35        # BIOS, -M q35, AHCI 根盘
make run-uefi-q35   # UEFI, -M q35, AHCI 根盘
make run-xhci       # BIOS, -M pc, qemu-xhci 键鼠(无 UHCI), 验证 xHCI HCD
make debug / debug-uefi   # gdb (tcp::26000)
```

- OVMF 固件默认取 `/opt/homebrew/share/qemu/edk2-x86_64-code.fd`(可用 `OVMF=` 覆盖)。
- 磁盘布局(`boot/mkboot.py hdd`): 扇区 0 = MBR(引导代码 + p1: FAT32 ESP(0x0C)
  + p2: ext3 rootfs(0x83)); 扇区 1 起为内核裸镜像(BIOS 路径); ESP 位于 1MB 处,
  内含 `BOOTX64.EFI` 与 `KERNEL.ELF`。**ext3 分区必须放在分区表最后一个非空项**,
  内核按此定位 rootfs(`get_rootfs_entry`)。
- 附加存储设备验证: 运行时追加
  `-device ahci,id=ahci0 -drive id=dsata,file=...,if=none -device ide-hd,drive=dsata,bus=ahci0.0`
  与/或 `-device nvme,serial=ewok0 -drive id=dnvme,file=...,if=none`(配合
  `-device nvme-ns,bus=nv0,drive=dnvme`)。

## ISO 启动镜像 (Ventoy / CDROM)

```bash
cd machines/x86/system && make basic && make sd-iso   # 128MB 精简 rootfs
cd machines/x86/kernel && make iso                    # 生成 ewokos.iso
make run-iso          # BIOS (SeaBIOS): El Torito no-emulation, 仅内核日志
make run-iso-uefi     # UEFI (OVMF): 内核 + 内存盘 rootfs, 完整启动到登录
```

- ISO 布局(`boot/mkboot.py iso`, 无需 xorriso/genisoimage): 扇区 0 = MBR(内核
  起始 LBA 经 "EWOK" 补丁槽指向文件区 KERNEL.IMG, U 盘 dd/GRUB chainload 可引导);
  PVD@16 + El Torito 引导目录@19: BIOS 默认入口 = **无仿真**(装载 `boot/cd_stage1`
  2KB + 扁平内核到 0x7C00, stage1 在 PM 下把内核搬到 0x100000, 与 Linux live ISO
  同款); EFI 入口 = 无仿真 FAT ESP 镜像(仅 BOOTX64.EFI); ISO9660 根目录含
  `KERNEL.IMG;1`/`KERNEL.ELF;1`/`ROOTFS.IMG;1`。
- UEFI 引导时 `BOOTX64.EFI` 在 ExitBootServices 前把 `ROOTFS.IMG` 拷入 RAM
  (≤512MB 物理地址上限), 经 bootinfo v3 传给内核; 内核 `bsp/sd.c` 与用户态
  `bsp_sd.c` 各自提供 ramdisk 后端(sdfsd 照常挂载, 写落 RAM 重启即失)。
  文件优先经 SimpleFileSystem 读取, 找不到时回退到 BlockIo 直读 ISO9660
  (Ventoy UEFI 不一定给 ISO 挂文件系统)。
- BIOS(Legacy) 引导只到内核日志: 无仿真装载的只有内核, 无 rootfs, 预期为
  `sd init` 失败(ATA/AHCI/NVMe 全空)后 `loading init [failed!]` 停机, 属正常。
- 真机加固(相对 QEMU 的差异, 已在仿真中验证):
  - BIOS 对 no-emulation 的装载量实现不一 (规范只保证装载 loader 本身,
    ISOLINUX 同样只信任 2KB): `cd_stage1` 装载后校验内核入口魔数, 缺失
    (真机 BIOS 截断) 时经 INT13h CD 扩展读自行装载 (2KB LBA 由 mkboot
    补丁进 stage1 的 "1CDK" 槽); PM 搬运后二次校验可拦截 A20 失效。
  - INT15h E820 捕获 (cd_stage1, BIOS-CD 路径): 内核按真实内存上限工作,
    小内存机器不再误用 512MB 默认值; 注意 BIOS 硬盘/USB 路径 (bios_boot.S
    预算不足) 仍为 512MB 假设。SeaBIOS 不推进 E820 的 DI, 捕获代码自带
    24B 步长转存。
  - UEFI: ROOTFS.IMG 分配强制 2MB 对齐 (内核 2MB 大页窗口要求,
    `_rd_ptr` 另有非对齐兜底); `check_mem_map_arch` 放行 bootinfo 上报的
    GOP fb 区间 (真机固件可能给 <0x80000000 的帧缓冲)。
  - 内核内存布局按 E820 总量取整: `ALLOCABLE_PAGE_DIR_SIZE` 页对齐
    (非规整内存容量如 255MB 曾导致 kmalloc 基址跨页 Panic)。
- 真机差异已处理: x2APIC 交接 (每核 x86_lapic_mode_fixup 按 SDM 降回
  xAPIC, MADT type 9 条目解析, ID>255 跳过); 24bpp GOP 帧缓冲
  (fbdisplayd blt24); X 会话早于 displayd 就绪时 `random_to(0)` 除零
  崩溃 (basic_math 防护); GOP-only 机器 (GPD 等 UEFI-only 设备) 内核
  控制台 —— vgacon 新增 GOP 帧缓冲后端 (8x16 点阵字体, 高分屏 2x,
  X86_FB_VA 2MB 大页窗口, PDPT[2] 共享), bootinfo 上报的 fb 是唯一
  可见输出; fb_pitch 修复 (GOP Mode Info 的 PixelInformation 是
  16 字节 BITMASK, 之前按 4 字节解析导致 fb_pitch=0); 用户态 displayd
  fb VA 重排 (sys_dma 末尾 0xB2000000 落在内核 ramdisk VA 窗口
  0xB0000000-0xB7FFFFFF 内 -> data abort, 移至 0xB8400000), vgacond
  GOP 后端 + /dev/vga0 图形会话 + keyb0 键入转发 (GPD 实测卡光标
  全链路修复, 真机回归点)。
- 遗留风险 (真机待验): 关 CSM 机器的 0xB8000 文本区可能不接显示
  (vgacon 仅镜像, 不影响串口); BIOS 硬盘/USB 路径 (MBR) 内存探测为
  512MB 假设 (<512MB 内存的老机器不适用, CD 引导路径有 E820)。
- Ventoy 实机使用: 将 `ewokos.iso` 放入 Ventoy U 盘, UEFI 模式可完整启动;
  Legacy 模式仅内核日志。真机 BIOS 兼容性未验证(SeaBIOS/OVMF/QEMU 已实测)。
- 真机排障 (启动日志全程 VGA 可见, 无串口也能定位卡点):
  - UEFI: stub 日志双输出 (串口 + ConOut 固件控制台), 含文件搜索/SFS->
    ISO9660 兜底/内存盘装载各阶段; EBS 后跳内核前在文本屏右上写 "K"。
  - BIOS-CD: stage1 在屏幕底行 (24,40) 起打标记: '1'=stage1 入口,
    '8'=E820 完成, 'R'=INT13h 回退装载中, '.'=每 32KB 块进度; 内核启动
    后清屏转入内核日志 (kout->vgacon 镜像全程可见)。
- 回归: ISO 路径的 ramdisk 保留通过 `kalloc_arch` 在空闲页表挖洞实现,
  `mem_map` 门禁(`check_mem_map_arch`)放行 ramdisk 区间供 sdfsd 映射;
  两条引导路径已固化为 `scripts/iso_test.exp`(UEFI 登录+shell 回环,
  BIOS 内核日志到 sd init 失败/loading init 失败停机), 见"回归测试"矩阵 #9。
  改动 rootfs 后重建顺序: `make basic && make sd-iso`, 然后
  `rm ewokos.iso boot/uefi/esp_iso.img && make iso`。

## system/ 侧驱动总览

| 总线/设备 | 驱动(路径) | 说明 |
|---|---|---|
| USB (UHCI) | `system/libs/bsp/src/bsp_usb.c` + `uhci.c`(x86 HCD) + `system/basic/drivers/usbhostd` + `hid_keybd/hid_moused/hid_touchd` + `usbfat32fsd` | 平台无关 usbhostd 枚举 + x86 UHCI 轮询 HCD; QEMU `-usb -device usb-kbd` 即可验证 |
| USB (xHCI) | `system/libs/bsp/src/xhci.c`(自 raspi5 `arch_bcm2712/xhci.c` 移植) + `bsp_usb.c` 双控制器扁平端口层 | PCI(class 0C/03/30)→ 64 位 BAR0 经 SYS_MEM_MAP 映射 → 全轮询事件环; 支持 slot 寻址/控制传输/中断 IN(HID 键鼠)与 hub(TT/route string)。UHCI 端口索引保持不变, xHCI 端口追加其后, 两类控制器共存于同一扁平端口空间; 批量(bulk)/MSC 仍走 UHCI(xHCI 驱动尚无 bulk)。QEMU `-device qemu-xhci` 验证 |
| SATA (AHCI) | `system/libs/bsp/src/ahci.c`(用户态 HCD) + `drivers/x86/atafsd` | PCI 探测(class 0106)→ABAR 映射→轮询 DMA 读/写/flush; atafsd 在其上挂 ext3; **另提供内核态 AHCI 引导路径**(见 `machines/x86/kernel/bsp/sd.c`) |
| NVMe | `system/libs/bsp/src/nvme.c` + `drivers/x86/nvmefsd`(自 raspi5 移植, 平台无关) | **全链路实测通过**: PCI 探测→BAR0 映射→控制器使能→IDENTIFY→IO CQ/SQ 创建→ext3 挂载 `/mnt/nvme`→文件数据回读。修复过程修正了 CQE/CDoorbell/CDW 打包、返回值语义等多个 bug, 详见 git 历史 |
| ATA (IDE) | 内核 `bsp/sd.c` + 用户态 `bsp_sd.c` | **全位置探测**(primary/secondary × master/slave, 首个有盘位置作为根盘) + **LBA48**(0x24/0x34); 无 legacy IDE 时回退 AHCI。实测: 根盘置于任一 IDE 位置均可引导 |
| 显示 | `system/libs/bsp/src/bsp_fb.c` + `drivers/x86/fbdisplayd` | UEFI 启动时直通 GOP framebuffer(sysinfo.fb); BIOS 时用 BOCHS DISPI |
| 输入 | `drivers/x86/ps2keybd ps2moused`, ttyd(COM1) | |
| 电源 | `drivers/x86/powerd` | |
| 内核 VGA console | 内核 `bsp/vgacon.c`(kout 第二输出汇点, uart_write 镜像) | 引导阶段日志/挂死点在 VGA 可见: VA 0xB8000 恒等页在 boot_pml4/内核表/任务表三阶段齐备 (任务表经 clone_kernel_vm -> arch_proc_low_vm); uart_write 有限轮询 (无串口平台不再挂死); loading init 完成后 console_handoff() 内核停写 |
| 用户态 VGA 控制台 | `drivers/x86/vgacond`(/dev/vga0, system 侧接管) | console_handoff 后接管屏幕续写: 直接写 VA 0xBE000000 (内核为所有任务建立的用户可写恒等页, 物理地址 PC 标准 0xB8000), 从内核最后一屏定位续写; 输出侧供 session/login 等使用 (/dev/vga0), 输入侧 (键盘) 待接 |

init.rd(`system/etc/basic/init.rd`)按"探测失败自动退出"的方式后台拉起
atafsd/nvmefsd, 无对应硬件时不影响启动。

## 硬件虚拟化（KVM，Arch x64 等宿主）

镜像与构建宿主无关（macOS/Linux 产物通用）。在开启 VT-x/AMD-V 的 Linux x64
宿主上可用 KVM 加速（TCG 下 NVMe 首命令完成延迟等模拟器时序问题在 KVM 下预期消失）：

```bash
# Arch: sudo pacman -S --needed qemu-full edk2-ovmf
make run ACCEL=kvm          # BIOS + KVM
make run-uefi-kvm           # UEFI + KVM
# 或用现成脚本(含依赖/KVM 可用性检查):
scripts/run-arch-kvm.sh kernel.hdd uefi    # uefi|bios|q35|q35-uefi
```

无 KVM 的宿主默认 `-accel tcg`，行为与此前一致。

**Apple Silicon (M1/M2/M3) Mac 上的说明**：无法用 "Rosetta 2 运行 x86 QEMU +
HVF 加速" 的组合——已实测（最小探针程序 `hv_vm_create`）：Apple Silicon 的
Hypervisor.framework 对 x86_64 VM 直接返回 `HV_UNSUPPORTED (0x4)`，与进程是否
经 Rosetta 转译无关（框架按宿主架构虚拟化）。AS 上 x86 客户机只能 TCG；
Rosetta 跑 x86 QEMU 属双重转译（Rosetta 转译 TCG 生成代码），比原生 arm64
TCG 更慢，不推荐。

## 回归测试

```bash
# 需 expect; 先 make + make run 的产物(kernel.hdd)就绪
expect scripts/boot_test.exp bios|uefi   # 引导->登录->shell->命令回环
expect scripts/periph_test.exp           # USB/SATA/NVMe 外设: 登录后逐项挂载+数据回读
expect scripts/xhci_test.exp             # qemu-xhci 键鼠(xHCI HCD) + UHCI MSC 盘共存
                                         # 含端到端: QMP 按键/mouse_move -> /dev/keyb0|mouse0
expect scripts/iso_test.exp              # ISO 双路径: UEFI 完整启动(内存盘 rootfs) +
                                         # BIOS 内核日志到 sd init 失败(设计内)
expect scripts/iso_matrix.exp bios|uefi  # ISO 硬件矩阵(9 配置): 机型/固件/内存/
                                         # SMP/附加数据盘组合, 结果在 /tmp/iso_matrix/
expect scripts/grub_test.exp             # GRUB 2.14 救援 CD chainload 引导 ISO 的 MBR
                                         # (需 grub-rescue-cdrom.iso, 下载命令见脚本头)
# IDE 位置测试(内核日志会打印 sd: ide pos=N base=..):
qemu ... -drive file=kernel.hdd,if=ide,index=2,media=disk ...   # secondary master
qemu ... -drive file=kernel.hdd,if=ide,index=1,media=disk ...   # primary slave
```

注: OVMF/TCG 下 BDS 枚举偶发先于 ESP FAT 挂载而落入 UEFI Shell, 测试脚本
已内置回退(Shell 中直接启动 `\efi\boot\bootx64.efi`), 对该竞态免疫。
xhci_test 的按键注入用 `scripts/qmp_key.py`(QMP input-send-event 按住
400ms 再释放): HMP `sendkey` 的按下/释放只隔几 ms, 会被 hid_keybd 的
"只保留最新快照"耗尽模型塌缩掉, 真实键盘的按下持续几十至几百 ms 不会。

### 兼容性矩阵实测 (scripts/matrix_run.sh, TCG)

| # | 配置 | 结果 |
|---|------|------|
| 1 | pc + BIOS + IDE primary master | PASS |
| 2 | pc + UEFI + IDE primary master | PASS |
| 3 | pc + BIOS + IDE secondary master | PASS |
| 4 | pc + UEFI + IDE secondary master | PASS |
| 5 | pc + BIOS + IDE primary slave | PASS |
| 6 | q35 + BIOS + AHCI | PASS |
| 7 | q35 + UEFI + AHCI | PASS |
| 8 | pc + BIOS + xHCI 键鼠 + UHCI MSC 盘 (xhci_test.exp, 5 项含键鼠端到端) | PASS |
| 9 | ewokos.iso 双路径 (iso_test.exp): UEFI 完整启动到 shell + BIOS 内核日志到 sd 失败 | PASS |
| 10 | GRUB 2.14 救援 CD chainload 引导 ISO MBR (grub_test.exp), ISO 以硬盘形式挂载 | PASS |
| 11 | ISO 硬件矩阵 (iso_matrix.exp, 9 配置): BIOS pc/q35 × 512/256MB(3);
     UEFI pc/q35 × 1GB/512MB × SMP4 × 附加 IDE/AHCI 数据盘(6), 同一份 ewokos.iso | PASS |

**10/10 PASS**。已知注意事项:
- q35 下 sdfsd/atafsd 等 init 守护进程的 AHCI 重初始化在 TCG 下 ~90s/个,
  登录需数分钟(KVM 即恢复常速); 基础 init.rd 不启动 atafd —— 根盘位于
  AHCI 时它会与 sdfsd 双挂载同一 HBA 端口造成死锁, SATA 数据盘测试请用
  periph_test.exp (pc + 独立 ahci0 设备, 登录后显式 `bgrun atafsd /mnt/sata`)。
- xHCI 键盘端到端在 TCG 下依赖 hid_keybd 耗尽节奏(见其 snapshot klog 注释),
  TCG 下偶发服务不到按键; KVM/真机时序余量大几个数量级, 预期消失。
- 内核 VGA console 覆盖引导阶段(至 loading init 完成、console_handoff);
  用户态启动后的内核日志仍只走串口 —— 守护进程 svc 上下文的 VGA 写会触发
  用户态堆边界问题(已见 /sbin/core data abort), 待查后可移除 handoff。
- GRUB chainload 注意: GRUB 2.14 把它自己的引导盘号传给 DL, `chainloader`
  前必须 `set root=(hd0)`(见 grub_test.exp 头注)。ISO 同时可作为裸硬盘
  引导(`-drive file=ewokos.iso,media=disk -boot c`, 同一份 MBR); 引导扇区
  的 CHS 回退几何经 INT13h AH=08 实测, AH=42 扩展读失败自动回退 CHS。

外设测试前置（一次性）:
```bash
dd if=/dev/zero of=sata_test.img bs=1048576 count=64   # ext3, 含 sata_marker.txt
dd if=/dev/zero of=nvme_test.img bs=1048576 count=64   # ext3, 含 nvme_marker.txt
dd if=/dev/zero of=usb_test.img  bs=1048576 count=64   # FAT32, 含 usb_marker.txt
#  ext3 两个: mke2fs -q -t ext3 -b 4096 -I 128 -F <img> && e2cp <marker> <img>:/
#  FAT32  : mformat -F -i <img> :: && mcopy -i <img> <marker> ::/
```

实测结果 (TCG): **USB 3/3**、**SATA 2/2**、**NVMe 1/1** —— 全部通过
(枚举→挂载→标记文件数据回读; NVMe 挂载时延在 TCG 下波动较大, 测试内置重试)。

## 本轮移植涉及的关键文件

```
machines/x86/kernel/boot/uefi/          UEFI bootloader(main.c/efi_stub.S/
                                        efi.h/bootinfo.h/linker.ld/mkpefi.py)
kernel/platform/x86/arch/x64/boot.S     .uefi 段(64 位 UEFI 入口) + 备用 PD
machines/x86/kernel/bsp/hw_info_arch.c  bootinfo 解析 + AHCI 早期探测/映射
machines/x86/kernel/bsp/sd.c            ATA PIO + 内核 AHCI 回退(引导路径)
machines/x86/kernel/bsp/x86_bootinfo.h  bootinfo 协议
machines/x86/kernel/boot/mkboot.py      双路径磁盘布局(BIOS 段 + ESP + ext3)
machines/x86/system/libs/bsp/src/ahci.c 用户态 AHCI(SATA) HCD
machines/x86/system/libs/bsp/src/nvme.c 用户态 NVMe HCD
machines/x86/system/libs/bsp/src/xhci.c 用户态 xHCI HCD(raspi5 移植; x86 适配:
                                        mfence 屏障、DMA 总线地址=物理、
                                        定宽 32 位 MMIO 访问器 —— GCC 会把
                                        `get32(x)&BIT` 收窄成单字节 MMIO 读,
                                        qemu-xhci(min_access_size=4)下读到错位
                                        字节, HCH 永远观察不到)
machines/x86/system/libs/bsp/src/bsp_usb.c
                                        UHCI+xHCI 双控制器扁平端口模型、
                                        PCI 探测/BAR 映射、MSC 挂载 fork 推迟
                                        到 bsp_usb_poll(修复 /dev/hid0 注册前
                                        spawn 的竞态)
machines/x86/system/drivers/atafsd/     AHCI 上的 ext3 服务
machines/x86/system/drivers/nvmefsd/    NVMe 上的 ext3 服务
kernel/kernel/src/hw_info.c             sys_dma 物理基址页对齐(dma_phy_addr 修正)
system/basic/drivers/usbhostd/usbhostd.c 枚举日志走 klog(slog→klog, 热路径仍静默)
system/basic/drivers/hid_keybd/          耗尽节奏注释(TCG 下按键快照可见性)
```
