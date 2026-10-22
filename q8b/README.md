# EwokOS Q8B 移植（Radxa Dragon Q8B / Qualcomm SC8280XP）

本目录是 EwokOS 面向 **瑞莎 Dragon Q8B**（Snapdragon 8cx Gen 3 / SC8280XP，
4×Cortex-X1 + 4×Cortex-A78，Adreno 690）的板级移植。

第一里程碑目标：**从板载 EDK2 UEFI 启动一个 AArch64 EwokOS 内核，串口输出内核
日志，挂载 UEFI 载入内存的 ext3 根文件系统，启动到 `core/vfsd/sdfsd` 与
串口登录；并可在 GOP 帧缓冲上启动图形控制台。**

Q8B 没有 U-Boot：SPI NOR 里是完整的 EDK2 UEFI，直接加载 `.efi`。因此移植的
启动链是：

```
SPI NOR EDK2 UEFI
  └─ BOOTAA64.EFI（本目录 kernel/boot/uefi）
       ├─ 通过 SimpleFileSystem / EFI_LOAD_FILE / EFI_LOAD_FILE2 读入
       │    KERNEL.BIN 与 ROOTFS.IMG
       ├─ 取 GOP 帧缓冲、UEFI 内存图、DTB（如有）
       ├─ 把内核放回物理 0x80080000，收集最终内存图
       ├─ ExitBootServices
       ├─ 在 0x80070000 发布 bootinfo（固定物理地址）
       └─ 跳到 0x80080000
            └─ EwokOS kernel（.init 物理链接 + 高半区 VMA）
                 ├─ UART17 (GENI) 控制台
                 ├─ GICv3 + ARM generic timer
                 ├─ PSCI CPU_ON（8 核）
                 └─ RAM disk ext3 rootfs or microSD ext3 read-only rootfs
                      └─ /sbin/init
```

## 当前状态

| 模块 | 状态 | 说明 |
|---|---|---|
| UEFI loader（BOOTAA64.EFI） | ✅ 已实现，QEMU/EDK2 实测 | SFS + LoadFile + LoadFile2，GOP/内存图/DTB，EBS，16KB 对齐 |
| AArch64 内核启动 | ✅ 已实现，QEMU/EDK2 实测 | RAM_BASE 感知链接脚本、早期页表、early vector、bootinfo |
| RAM disk ext3 rootfs | ✅ 已实现，QEMU 实测 | UEFI 载入 ROOTFS.IMG，内核 + 用户态 `sdfsd` 共用 |
| 串口控制台 UART17 (GENI) | ✅ 内核 + 用户态驱动，待上板验证 | 0x00884000，115200 8N1，TX 已按 Linux/qcom_geni 时序实现 |
| GICv3 / 8 核 SMP | ✅ 已实现，待上板验证 | GICD 0x17a00000 / GICR 0x17a60000，PSCI SMC CPU_ON |
| GOP 帧缓冲 | ✅ 已实现，QEMU headless 下未验证显示 | 由 stub 记录地址/分辨率/格式，fbdisplayd 直写 |
| GUI 控制台 | ✅ 已构建（displaymand/consoled/fontd/fbdisplayd） | 无输入设备，先做显示；串口可登录 |
| microSD 高速读写 | ✅ 完整验证 50MHz HS（ADMA2 多块 16MB/s） | SDCC2 **4-bit / 50MHz（SD High Speed）**：CMD6 切换函数 1（64 字节状态块）、GCC `0x0605`、HOST_CONTROL HS 位。控制器 `caps0=0x362dc8b2`：**ADMA2=1、SDMA=0**，所以旧 SDMA 路径必挂。用户态走 **ADMA2 多块**（`SDCC2_USE_ADMA=1`、`SDCC2_FORCE_SINGLE_BLOCK=0`），失败回退 PIO 单块；PIO 多块仍禁用（16 块静默错）。实机 SD 根盘 `sdprobe test`：32 块 16KB **16.1MB/s**，2/4/8/16/32 块 `cmp=0`，ADMA 写 4 块/回读/恢复通过；内核 init 仍走 PIO 单块。 |
| **M.2 NVMe（PCIe2a x4）** | ✅ **用户态驱动实机验证，读 1.77 GiB/s** | 见下文「M.2 NVMe（PCIe2a，用户态驱动）」：ECAM 枚举 + BAR0 映射 + 轮询 admin/I/O 队列；Lexar 1TB SSD（126f:2263，NVMe 1.3，MDTS=6/256KB）。原始读 **1.77 GiB/s**（256MB，256KB×QD≥4），带拷贝的实用路径 **~1.2 GiB/s**；读取 MBR/分区表/EXFAT 引导扇区与真实盘上数据比对通过。 |
| UFS 根盘 | ⏳ 未实现 | 后续接 UFS 3.1 |
| USB xHCI / 键鼠 | ✅ 已实现（xhci + HID），实机枚举 | DWC3 xHCI @0x0a400000，轮询队列；键鼠 daemon 已接入 |
| QPS615 双 2.5GbE | ✅ 已实现（TC956x/XGMAC3） | pcie3a + TC9563 交换机，实机 DHCP/ping/ssh 可用；批量传输稳定性待修 |
| Adreno 690 / freedreno | ✅ 2D/API/显示集成实机通过 | 默认启动 g2dd；g2dtest 0 failure；完整桌面新启动流程待验收 |

> 说明：QEMU 只能验证通用 AArch64 路径（UEFI → 内核 → 页表 → ext3 → init →
> core/vfsd/sdfsd）。QCOM 专属的 GENI UART、GICv3、PSCI、UFS、PCIe、GOP 需要
> 在 Q8B 实机验证。仓库内的 `boot/uefi/test/` 有两个 QEMU 回归脚本。
>
> **SD 高速链路的关键点**（对齐 Linux `sdhci` / `sdhci-msm` / `mmc_sd_switch`）：
> 1) CMD6 状态块是 **64 字节（512 bit）**，按 512 字节读会 data timeout，
>    HS 永远切不过去；2) `gcc_sdcc2_apps_clk_src` 的父时钟
>    `GPLL0_OUT_EVEN=300MHz`，25MHz=`0x060b`、50MHz=`0x0605`（旧值 `0x0617`
>    实际只有 12.5MHz）；3) 本 SDCC2 的 CAPABILITIES bit6=0，**不支持
>    Auto CMD12**，`TRANSFER_MODE` 多块读用 `0x32` 并在完成后手动 CMD12；
>    4) PIO 多块读不能逐块清 `BUFFER_READ_READY`，否则第 2 块后停住。
>    内核把总线留在 **50MHz/4-bit HS**，用户态 `sdcc2.c` 继承该状态；
>    **旧 rootfs 的 sdfsd 未带这些修复，更新内核后必须重新构建 rootfs**，
>    否则用户态 ext3 挂载会失败（`ext3_init_ex2=-1`）。sdfsd 必须链接新的
>    `libbsp.a`；用户态 HS 固定为 PIO 单块，**不要打开 SDMA**（50MHz 下会
>    挂住 SDCC2 总线）；多块 PIO 在 16 块开始出现数据不一致，因此内核和
>    用户态都固定单块（`sdfsd` 的读路径与 `sdprobe full` 验证的一致）。
>
> **SD 驱动完整验证（PXE ramfs，`sdprobe full`）**：
> - 1MB 单块读：`us=248828`，`4115 KB/s`；
> - CMD18 多块：2/4/8 块通过，16 块 `memcmp=114`（静默错），32 块失败回退；
> - CMD24 写 4 扇区到未分配区 LBA 30537216：写/回读 `cmp=0`、恢复 `rc=0`；
> - GPT/ext、HS 50MHz/4-bit（`host-ctrl=0x0d06`）均正确。
>
> **ADMA2（最终方案）**：`caps0` 明确 ADMA2=1、SDMA=0。用户态用 ADMA2
> 32 块（16KB）读，实机 SD 根盘实测 **16.1MB/s**（990us/16KB），2/4/8/16/32
> 块全部 `cmp=0`；ADMA 写 4 块、回读、恢复通过。PIO 只在 ADMA 失败时
> 单块回退，绝不使用 PIO 多块。
> 最终只启用单块 PIO；`sdfsd` 与内核 init 加载都按此配置编译。

## 关键地址

| 项 | 值 | 来源 |
|---|---|---|
| DRAM base | `0x80000000` | sc8280xp.dtsi `memory@80000000` |
| 内核物理加载地址 | `0x80080000` | 本移植（与 Linux arm64 一致） |
| bootinfo 物理地址 | `0x80070000`（64KB） | 本移植，stub 写入、内核读取 |
| 内核高半区 VMA | `KERNEL_BASE + (phys - RAM_BASE)` | `kernel/mkos.lds.S` |
| UART17 | `0x00884000`，115200 8N1，GIC SPI 583 | sc8280xp.dtsi `serial@884000`；40-pin Pin6/8/10 |
| GIC | GICD `0x17a00000`，GICR `0x17a60000`（stride `0x20000`） | sc8280xp.dtsi `interrupt-controller@17a00000` |
| 可用 DRAM（首里程碑） | 8GB（按 UEFI 内存图过滤固件占用区，可分配约 7.2GB） | 实机内存图已验证 |
| GOP 帧缓冲 | 由 UEFI 报告，实机地址待上板确认 | stub 日志会打印 `gop WxH bpp pitch @addr` |

## 构建

### 工具链

需要 `aarch64-none-elf-*`（GCC binutils）。本机路径：

```bash
export PATH=/Users/tony/toolchain/arm-gnu-toolchain-14.3.rel1-darwin-arm64-aarch64-none-elf/bin:$PATH
```

根文件系统成像还需要 `mke2fs`、`e2cp`（Homebrew e2fsprogs），ESP 制作需要
`mtools`（`mformat`/`mcopy`/`mmd`）。

### 一键构建

```bash
cd machines/q8b
make payload
```

产物：

- `kernel/kernel.bin` — 内核裸镜像
- `kernel/boot/uefi/BOOTAA64.EFI` — UEFI 引导器
- `root_aarch64.img` — 112MB ext3 根文件系统（RAM disk 用）

分步：

```bash
make -C machines/q8b/kernel            # 内核
make -C machines/q8b/kernel efi        # UEFI 引导器（RAM disk）
make -C machines/q8b/kernel efi SD_ROOTFS=1 FORCE_UART17=1  # 从 microSD ext3 启动
make -C machines/q8b/system x          # 默认完整桌面，启动 g2dd 后启用渲染
make -C machines/q8b/system gui        # 可选：basic + 图形控制台（同样启用 g2dd）
make -C machines/q8b/system sd         # ext3 rootfs
```

## 启动方式

### 方式 A：USB ESP（首推，最省事）

```bash
cd machines/q8b
SD_ROOTFS=1 ./deploy.sh # ESP/TFTP 只放 loader+kernel，rootfs 用 microSD ext3
# 生成 build/q8b-esp.img（384MB，MBR + FAT32 ESP，ESP 从 1MiB 开始）
sudo dd if=build/q8b-esp.img of=/dev/rdiskN bs=1m
```

把 U 盘插到 Q8B，开机按 **F12** 选 U 盘；或 F2 → Boot Maintenance Manager →
Boot From File → `\EFI\BOOT\BOOTAA64.EFI`。引导器会在同一 FAT 卷根目录找
`KERNEL.BIN` / `ROOTFS.IMG`（也尝试 `\q8b\` 和 `\EFI\BOOT\`）。

### 方式 B：PXE

`deploy.sh` 会把三个文件复制到 `q8b/boot/tftp/q8b/`。把 DHCP 的 bootfile
指向 **`q8b/BOOTAA64.EFI`**（直接 NBP，最可靠）：

- 修改 `q8b/boot/dnsmasq-direct.conf` / `dnsmasq-lan.conf` 的
  `dhcp-boot=...` 为 `q8b/BOOTAA64.EFI`；
- 或保持 GRUB 菜单，选 `Q8B custom UEFI app (chainload BOOTAA64.EFI)`。
  注意：GRUB chainload 时 `KERNEL.BIN`/`ROOTFS.IMG` 仍通过固件的
  EFI_LOAD_FILE 读取；如果该句柄不可用，请改用直接 NBP。

引导器按以下顺序找文件（SFS 或 LoadFile）：

```
\q8b\KERNEL.BIN, \KERNEL.BIN, \EFI\BOOT\KERNEL.BIN
\q8b\ROOTFS.IMG, \ROOTFS.IMG, \EFI\BOOT\ROOTFS.IMG
```

若都找不到，再尝试 Linux initrd media（GRUB `initrd` 暴露的 LoadFile2）。

### 串口

40-pin：Pin6=GND，Pin8=TXD，Pin10=RXD（SoC UART17），115200 8N1，
USB-TTL **不要接 VCC**。

UEFI 阶段日志：F2 → Radxa Platform Configuration → Debugging Settings →
`Synchronous Debug UART in UEFI` = Enabled，`Debug Print Level` = Debug。

`BOOTAA64.EFI` 真机只走串口（优先 EFI Serial I/O，115200 8N1；协议不可用时
直写 UART17 GENI）。UEFI ConOut 也接同一 UART，双写会让每行重复两次，所以真机
不再同时写 ConOut；QEMU 无串口后端，仍走 ConOut。启动第一行是
`[uefi] serial log: EFI Serial I/O` 或 `[uefi] serial log: UART17 direct`。

如果启动第一行是 `[uefi] serial log: EFI Serial I/O` 但 40-pin 没有输出，
说明 EFI Serial I/O 指向了别的 UART（例如 EUD COM）：用
`make efi KERNEL_ONLY=1 FORCE_UART17=1` 重编，强制直写 UART17。

### 预期启动日志（节选）

```
[uefi] serial log: EFI Serial I/O
=== EwokOS Q8B UEFI loader ===
[uefi] firmware rev 0x0000000000020046
[uefi] gop 1920x1080 bpp=32 pitch=7680 @0x00000000c6200000
[uefi] try \q8b\KERNEL.BIN
[uefi]   SFS ok, 704784 bytes
[uefi] try \q8b\ROOTFS.IMG
[uefi]   SFS ok, 134217728 bytes
[uefi] kernel copied to 0x0000000080080000

q8b: _boot_start
q8b: pgt init done
q8b: identity mapped
q8b: high alias mapped
q8b: mmio mapped
q8b: gic mapped, enabling mmu
q8b: boot page tables on
q8b: kernel entry
q8b: bootinfo ok ram=0x... rd=0x.../0x8000000 fb=0x...
=== ewokos booting ===
kernel: init sd                ... sd: ramdisk 112MB @ ...
kernel: loading init ... [ok]
[init process started]
init: /sbin/core         [ok]
init: /sbin/vfsd         [ok]
init: /sbin/sdfsd        [ok]
```

## 风扇控制

`fand` 通过 **GPIO118 软件 PWM（50 Hz）** 控制风扇，默认 **level 6 = 60%**，
支持手动档位：

```text
fan          # 查看当前档位/占空比
fan 3        # level 3 = 30%
fan 10       # level 10 = 100%
```

完整 rootfs 的 `/etc/init.rd` 已启用 `@/bin/ipcserv /drivers/q8b/fand /dev/fan`，
开机即按 60% 运行；手动 `fan` 会切到手动模式。

**TSENS 不要直接访问**：Q8B 的 TSENS 由 AOP 管理，当前 AOP 没有使能它
（`SROT_CTRL=0`）。此时读 TM 状态或写 SROT 去使能都会挂住互联总线并导致
复位（已实测）。因此安全版 `fand` 只做只读探测，检测到 `TSENS_EN=0` 时
不碰 TM/SROT，固定 60% 运行，`temp` 显示 `?`。

真正的 CPU 温度/自动风扇曲线需要 **Radxa SVC（ADSP GLINK）** 的
`cpu_temp_deci_c` / `pwm1` 接口，尚未接入。

## M.2 NVMe（PCIe2a，用户态驱动）

Q8B 有两个 M.2 M-key 槽：**pcie2a（Gen3 x4）** 与 **pcie3b（Gen3 x2）**。
PCIe 控制器、链路训练、iATU 与 BAR 分配都由固件在 `ExitBootServices` 前
完成；OS 侧只需要 ECAM 枚举 + BAR 映射 + 轮询 NVMe 队列，因此驱动完全位于
用户态（与 `bsp_eth`/`xhci`/`sdcc2` 同一模型）。

### 代码

| 文件 | 作用 |
| --- | --- |
| `system/libs/bsp/include/bsp/nvme.h` | 公开 API：`q8b_pcie_scan` / `q8b_pcie_link` / `nvme_open` / `nvme_read` / `nvme_write` / `nvme_read_stream` |
| `system/libs/bsp/src/nvme.c` | ECAM 枚举、BAR 翻译、admin/I/O 队列、identify、批量读流水线、128-bit 拷贝 |
| `system/drivers/nvprobe/nvprobe.c` | 实机探针：`scan` / `gpt` / `bench [MB]` / `verify [MB]` |
| `kernel/bsp/hw_arch.h` + `hw_info_arch.c` | `check_mem_map_arch()` 放行 pcie2a/3b 的 ECAM 与 BAR 窗口（`0x5_0000_0000` / `0x4_4000_0000`） |

### 地址与窗口（sc8280xp-radxa-dragon-q8b.dts）

| 槽 | ECAM（PCIe 配置空间） | 32-bit BAR 窗口（bus → CPU） | 64-bit BAR 窗口 |
| --- | --- | --- | --- |
| pcie2a | `0x5_0000_0000`（256MB） | `0x4000_0000` → `0x5_1000_0000` | `0x5_2000_0000` 恒等 |
| pcie3b | `0x4_4000_0000`（256MB） | `0x4000_0000` → `0x4_5000_0000` | `0x4_6000_0000` 恒等 |

用户态只映射总线 0–7（8MB ECAM）与 BAR0 的 16KB；BAR 值通过上表翻译成
CPU 物理地址。pcie2a/3b 的 DTS 没有 `iommus`，SMMU 旁路，所以
`sys_dma` 物理地址可以直接作为设备 DMA 地址（与 xHCI 相同）。

### 实机结果（Lexar 1TB SSD，126f:2263，NVMe 1.3）

```
root port link: speed=3 width=x4      # 8GT/s ×4 = PCIe Gen3 x4
  model   : Lexar 1TB SSD
  serial  : J38227J000468
  firmware: S0614B0
  nsze    : 1953525168 blocks x 512 B = 953869 MiB
  mdts    : 6 (max single transfer 256 KiB)
```

`nvprobe bench 256`（256MB，LBA 0 起顺序读）：

| 方式 | 结果 |
| --- | --- |
| 原始读（数据留在 DMA arena，不拷贝） | **1772 MiB/s**（128KB/256KB，QD≥4）；QD1 约 1500–1700 MiB/s |
| 带拷贝的实用路径（拷入 cached malloc 缓冲） | **~1220–1250 MiB/s** |
| 数据一致性 | `nvprobe verify` QD1 与 QD32 两次读 **IDENTICAL** |

`nvprobe gpt` 通过驱动读真实盘上数据：LBA0 是 MBR（`0xAA55`），
part1 `type=0x07 lba=2048 sectors=1953457584`、引导扇区 `fs=EXFAT`，
part2 `type=0xEF`（32MB ESP）。

### 加速要点

1. **队列基址必须 4KB 对齐**：NVMe spec 要求 ASQ/ACQ/IO SQ/CQ 基址 4KB
   对齐。最初的紧凑布局（ACQ 在 ctrl+0x400）会让控制器收不到 admin 命令
   （identify 永远超时，CSTS 却一直 RDY）。每个队列独占 4KB。
2. **Identify Controller 的 NN 在 0x204**（不是 0x184）；SQES/CQES/MAXCMD
   在 0x200。读错会得到 NN=0。
3. **双缓冲读流水线**：两个 `depth` 批次的 arena 交替使用，前一批完成即
   提交后一批，并在后一批 DMA 进行时把前一批拷出（拷贝与 I/O 重叠）。
4. **128-bit 拷贝**：`sys_dma` 是 Normal Non-Cacheable，用 LDP/STP 128-bit
   配对拷贝比 libc 的 8 字节 word memcpy 快约 2.4 倍，把带拷贝路径从
   475 MiB/s 提到 ~1.2 GiB/s。
5. **不要填满 SQ**：一次提交 `qsize` 条会让 tail 回到原值，门铃不再表示
   新命令；批量深度保持 ≤ (qsize-1)/2。
6. **完成可能是乱序的**：共享 CQ 上属于另一个在飞批次的 CQE 必须按 slot
   计数保留，不能丢弃，否则会误判超时。

### 限制与注意

- **pcie3b 空槽不要预扫描**：槽位为空时固件会把对应 RC 下电，读它的 ECAM
  触发外部 abort（内核 `SError swallowed`，随后该用户进程 data abort）。
  `nvprobe` 默认只扫 pcie2a，只有显式 `scan3b` 才碰第二个槽。
- 当前是**轮询**驱动（不开中断），单进程占用一个核；`nvme_read_stream`
  的流水线需要调用者提供 DMA arena 大小内的 chunk×depth。
- 写入路径（`nvme_write`）已实现但未做实盘写测试：盘上是别人的
  EXFAT/ESP，避免误改。
- 根文件系统仍未切到 NVMe：后续可加 `nvmed` 块设备 daemon，或给 `sdfsd`
  增加 NVMe 后端（`libsd` 的 `sd_read_sectors` 已可替换）。
- **批量 SSH 传输会造成网卡 RX 卡死**（`/drivers/q8b/net` 在 1500 字节
  连续帧下停住，ping/ssh 全断，串口仍活）。实测用
  `scp -l 300~500`（限速 37–62KB/s）+ 32KB 分片可稳定上传；串口控制台
  （`tty0`，root/root）是可靠的备用命令通道。该问题与 NVMe 无关，待单独
  修 `bsp_eth.c` 的 RX 环。

### 构建与运行

```bash
cd machines/q8b
make -C system/libs/bsp            # 编 libbsp（含 nvme.c）
make -C system/drivers/nvprobe     # 编 nvprobe
# 上板：scp 到 /home（限速分片）后
/home/nvprobe scan        # 枚举 + 打开控制器 + identify
/home/nvprobe gpt         # 读 MBR/GPT 与分区引导扇区（只读）
/home/nvprobe bench 256   # 256MB 读吞吐（raw + copy）
/home/nvprobe verify 64   # QD1 vs QD32 数据一致性
```

上传/维护辅助：`system/drivers/espwrite` 可在板子上直接读写 SD 卡的 FAT32
ESP（`espwrite list|get|put|rm|boot`），用于更新 `BOOTAA64.EFI` /
`KERNEL.BIN`，并用回读校验；`make -C system/drivers/espwrite` 构建。

内核侧只需 `hw_arch.h`/`hw_info_arch.c` 的 pcie2a/3b 窗口放行；其余全部
在用户态。内核改动通过 PXE TFTP 的 `KERNEL.BIN` 生效（loader 的
`MTFTP_FIRST=1` 优先走网络）。

## PXE RAM-disk 测试（不刷 SD）

`boot/uefi` 支持两种 loader：

- `BOOTAA64.SD.EFI`：`SD_ROOTFS=1`，只从 PXE 取 `KERNEL.BIN`，rootfs 在 SD；
- `BOOTAA64.EFI`：`SD_ROOTFS=0`，取 `KERNEL.BIN` + `ROOTFS.IMG`，rootfs 走 RAM disk。

TFTP 根目录（`eud/pxe/tftp/q8b/`）当前文件：

| 文件 | 用途 |
| --- | --- |
| `BOOTAA64.EFI` | PXE RAM-disk loader（SD_ROOTFS=0） |
| `BOOTAA64.SD.EFI` | SD rootfs loader（SD_ROOTFS=1），替换 `BOOTAA64.EFI` 使用 |
| `KERNEL.BIN` | HS 内核 |
| `ROOTFS.IMG` | 24MB 最小控制台 rootfs（含 `sdfsd`/`sdprobe`） |

最小 rootfs 由 `.tmp/pxe-rootfs` 生成：只放 `sbin`、`bin`、`drivers`、`etc`
和 `sdprobe`，不含 GUI/字体/应用；PXE 传输约 100s。启动后登录 `root/root`，
运行 `sdprobe` 可验证 SD HS。完整 rootfs 镜像仍用
`make -C machines/q8b/system sd` 生成，写入 SD 时用 `BOOTAA64.SD.EFI`。

MTFTP 实测约 240KB/s；给 EDK2 MTFTP4 传 `blksize` 选项反而更慢。loader 里
另有一个 PXE-UDP 直连 TFTP 实现（`Q8B_TFTP_FAST`），RRQ 发送成功但该固件的
PXE UDP 收包路径不回数据，默认关闭；后续可改用 `EFI_UDP4` 协议实现窗口化
TFTP。

## 设计要点（与其它机器的差异）

1. **RAM_BASE 感知链接脚本**：Q8B 的 DRAM 从 `0x80000000` 开始。`.init`
   仍按物理地址链接（早期 `adr`/栈引用可用），其余段链接到
   `KERNEL_BASE + (phys - RAM_BASE)`，满足 `V2P/P2V` 约定。QEMU 测试变体通过
   `--defsym=phys_base=$(RAM_BASE)` 复用同一脚本。
2. **固定 bootinfo 物理页**：UEFI 应用不能可靠地把参数留在寄存器里穿过
   `ExitBootServices`，stub 在 `0x80070000` 写 bootinfo（内存图复制到 +0x1000），
   内核对固定地址读取。结构在 `kernel/bsp/hw_arch.h` 与 `boot/uefi/main.c`
   两处定义，必须保持一致。
3. **16KB 页对齐**：内核用 16KB granule；UEFI 只保证 4KB 页。stub 把
   `KERNEL.BIN`/`ROOTFS.IMG` 的缓冲区向上圆整到 16KB（多分配一页），否则
   `SYS_MEM_MAP` 映射会丢掉低 0x0-0x3FFF 的偏移（实测踩过）。
4. **RAM disk 双映射**：内核在 `arch_vm()` 里把 ramdisk 映射到
   `P2V(rd_base)`，必须在 `load_kernel_config()` 之前完成（`init_allocable_mem()`
   在更后面）；用户态 `bsp_sd` 再把同一物理区间映射到用户 VA `0x60000000`。
   `kalloc_arch()` 把 ramdisk 从页分配器里挖掉，避免被用户进程覆盖。
5. **GICv3**：平台层的 `gic.c` 是 GICv2；Q8B BSP 自带 `gic_v3.c`
   （GICD/GICR + ICC_* 系统寄存器），QEMU 测试走 GICv2 分支。
6. **GENI UART**：按 Linux `qcom_geni_serial` / u-boot 的 TX 时序实现，不改
   baud/时钟（UEFI 已配好 115200）；用户态 `uartd` 另实现 RX FIFO 轮询。
7. **GOP 像素格式**：stub 记录 `PixelFormat`（0=RGB，1=BGR，2=BitMask），
   `fbdisplayd` 在 RGB 时做红蓝交换；Q8B 预期 32bpp BGR。

## QEMU 回归

两个脚本都不需要 Q8B 实机：

```bash
cd machines/q8b
make test-loader   # loader: SFS -> EBS -> jump（mock payload）
make test-kernel   # 真实内核：UEFI -> 内核 -> ext3 -> core/vfsd/sdfsd
```

依赖：`qemu-system-aarch64`、EDK2 `edk2-aarch64-code.fd`、`mtools`。
`test-kernel` 使用 `kernel_qemu.bin`（`make -C kernel TEST_QEMU=1`），
把 RAM_BASE 改为 `0x40000000`、UART 改为 PL011、GIC 改为 GICv2；
QCOM 专属驱动不上机不验证。

## Adreno 690 bring-up

GPU 驱动在 `system/libs/adreno/`（`libadreno.a`）。正常启动由
`/etc/g2d.rd` 设置 `ADRENO_ENABLE=1`，通过 `ipcserv` 启动唯一的
`/drivers/q8b/g2dd /dev/g2d`，等待服务就绪后再启动显示服务和桌面。
`gui` 与 `x` 共用此配置；顶层 `make rootfs` / `make payload` 默认构建 `x`。
Graph 与显示库通过 g2dd 接口提交受支持的绘制与 GOP 拷贝；未实现的操作
返回不支持，由客户端执行 CPU 回退。已提交任务的超时/故障会停止调用方并
保留缓冲区，不能当作普通 CPU 回退。寄存器序列按 Linux v6.6/v6.12
`drivers/gpu/drm/msm/adreno` 转写，寄存器偏移来自 Mesa `a6xx*.xml`。

已实现并完成 2026-10-22 实机验证（细节见 `test/HANDOVER.md`）：

| 阶段 | 内容 |
|---|---|
| 时钟 | GCC `gcc_gpu_gpll0*` / `ddrss_gpu_axi` / `gpu_memnoc_gfx`；GPUCC `cx_gdsc` 上电、`gmu_clk_src`（GPLL0_DIV/1.5=200 MHz）、`hub_clk_src`（GPLL0/3=200 MHz）及 CX 叶子 branch；`gx_gmu` 在 GX 上电后再开 |
| SMMU | GPU/GMU 上下文、GPU 地址映射与缓存一致性，按 A690 stream 路由初始化 |
| GMU | `a660_gmu.bin` 块链 → CM3 ITCM/DTCM/icache → `fw_start` → HFI 三队列（cmd/response/debug）→ perf/bw/core_fw/start → 初始 `GX_BW_PERF_VOTE` → 轮询 GX 电源状态 |
| CP | ring + `a660_sqe.fw` 装载、官方签名 zap 经内核 SCM PAS 认证、安全模式退出、CP_MEM_WRITE 自检 |
| 2D/compute | A2D fill/blit，普通 GLSL compute 实现精确 alpha、缩放与旋转；g2dtest 完整通过（0 failure） |
| 显示 | Graph / g2dd / display IPC / fbdisplayd 到真实 720×720 GOP 的全帧与脏更新、桌面恢复逐像素通过 |

固件：A690 使用 **A660 的固件镜像**（mainline `a6xx_catalog.c` 的 a690 条目：
`fw[GMU]="a660_gmu.bin"`、`fw[SQE]="a660_sqe.fw"`；linux-firmware 中不存在
`a690_gmu.bin`）。`firmware/adreno/` 内的三个文件由 `make basic` 安装到
rootfs 的 `/etc/firmware/adreno/`，签名 zap 使用
`sc8280xp/LENOVO/21BX/qcdxkmsuc8280.mbn`；官方固件保持原样。
`adreno_fw.c` 还会搜索
`/etc/firmware/qcom/`、`/usr/lib/firmware/qcom/` 等兼容路径。

**2026-10-21 首次上板结果**：GPUCC/SMMU GMU CB1 均就绪，但 GMU 固件加载返回
`-2`——旧代码按 `a690_gmu.bin` 查找，该文件在 linux-firmware 中并不存在。
本版修正为 `a660_gmu.bin`，同时补上 Linux 时钟父链里 EwokOS 缺失的两段：
GCC 的 GPU GPLL0/NoC 分支，以及 CX GDSC 上电；GMU 启动后新增初始 DCVS vote
与 GX 电源轮询。

**当前边界**：GPU 初始化、签名 zap 认证及 2D/API/显示集成已实机通过。
完整 `x` 桌面与默认启动镜像已编译打包，新启动顺序尚未上板验证。
Gaussian、三角形及 raw physical 90/270 度旋转仍使用明确的客户端 CPU 回退；
geometry/完整 3D 管线待后续实现。每次物理冷启动只初始化一次 GPU；正常
启动已有 `/dev/g2d` 时，直接运行 `/bin/g2dtest`，不要再次启动测试 daemon。

## 下一步

1. **上板验证**：串口日志、GOP 地址/分辨率、GICv3 redistributor affinity、
   GENI RX、PSCI 8 核；`Q8B_USABLE_LIMIT` 已按 UEFI 内存图放宽到 8GB（可分配约 7.2GB）。
2. **持久化根盘**：使用 `SD_ROOTFS=1` 构建 loader 后，UEFI 只传递 kernel，
   kernel 初始化 SDCC2 并通过 GPT 选择具备受支持 feature 的 ext2/3 分区，
   启动期只读加载 init；运行期 SD 读写由用户态 SDCC2 驱动接管（内核不再提供
   SD 块 syscall）。当前 `orb:/dev/sda2` 已作为 ext3 rootfs
   验证，`/dev/sda1` 仅保存 UEFI loader 与 kernel。
   UFS 3.1（`ufs_mem_hc`）仍待移植。
3. **输入**：xHCI + HID 键鼠；随后启用 `xwin` 完整桌面。
4. **网络**：PCIe + QPS615 双 2.5GbE；WiFi/BT M.2。
5. **NVMe 落地**：`nvprobe`/`libbsp nvme.c` 已验证控制器与 1.77 GiB/s 读；
   下一步加 **`nvmed` 块设备 daemon**（或给 `sdfsd` 加 NVMe 后端），把
   NVMe 分区作为可挂载根盘，并支持写入；同时修 `bsp_eth.c` 的 RX 环批量
   传输卡死问题。
6. **GPU**：2D/API/真实 GOP 集成已通过，默认启动 g2dd 的完整桌面镜像已构建；
   下一步是冷启动验收默认桌面，再推进 geometry/3D。路线评估见
   `../q8b/freedreno项目调研与自研OS-GPU驱动可行性评估.md`。
7. **调试增强**：EUD（BIOS 已有选项）作为中期 SWD 调试通道；PXE 直连 + 串口
   已是当前最快迭代闭环。

## 相关文档

- `../../docs/wiki/24-q8b-porting.zh.md` — 移植参考与实证记录
- `../q8b/` — 资料：规格书、原理图、设备树、EUD、GPU 调研、PXE 环境
- `../q8b/boot/README.md` — PXE/dnsmasq 环境

## 实机首启排查

| 现象 | 优先检查 |
|---|---|
| 完全看不到 `=== EwokOS Q8B UEFI loader ===` | U 盘 ESP 是否为 FAT32 且含 `\EFI\BOOT\BOOTAA64.EFI`；F12 是否选对设备；BIOS Secure Boot 是否阻止未签名应用（开发板一般可关）；GOP/HDMI 或 UEFI 调试串口是否可见 |
| `FATAL: cannot reserve kernel RAM at 0x80080000` | 串口会打印该地址附近的内存图。若被 DTB/固件占用，改 `kernel/config.mk` 的 `LOAD_ADDRESS`（并同步 `bsp/hw_arch.h` 的 `Q8B_KERNEL_PHYS`/`Q8B_BOOTINFO_PHYS` 和 stub 的 `Q8B_KERNEL_PHYS`） |
| 看到 `q8b: gic mapped, enabling mmu` 后停住 | 页表/early vector 问题；先用 `make test-kernel` 确认通用路径，再检查 `RAM_BASE`/`phys_base` 是否一致 |
| stub 有日志、内核无日志 | 40-pin 接线（Pin6/8/10，115200 8N1，勿接 VCC）；BIOS `Synchronous Debug UART in UEFI` 是否 Enabled；UART17 的 QUP 时钟/引脚是否由固件初始化 |
| 内核到 `loading init` 后失败 | `ROOTFS.IMG` 是否完整、是否 ext3；确认它是 112MB 的 `root_aarch64.img`，不是别的镜像 |
| `core/vfsd/sdfsd` 失败 | 看 `init: read_fs failed` / `ext3_init_ex2` 日志；确认 `bsp_sd` 的 ramdisk 物理地址是 16KB 对齐（stub 已保证） |
| 显示黑屏但串口登录正常 | 看 stub 的 `gop WxH bpp pitch @addr`；确认 `fbdisplayd` 启动、`/dev/disp0` 存在；若 GOP 是 RGB 而非 BGR，`fbdisplayd` 已按 `format` 交换红蓝 |
| 4K（3840x2160）下 xfinder/xapps/xterm 能启动却没有窗口 | 内核 scattered SHM 窗口被 4K 画布耗尽：显示前后缓冲 2×31.6MB，窗口的 `ws_g`/`ws_g2`/`frame_g`/`backdrop` 各约 31.6MB（全屏时），旧 128MB 窗口放不下（实机 1080p 约 57MB，放大到 4K 约 230MB）。aarch64 已把 `SHM_MAX_SIZE` 提到 512MB（约 2 倍余量）；日志里可见 `xserverd: window ... shm ... failed` 和 `xwin_open: no window canvas (...)`。另注意 `xapps` 打开后立即 `fullscreen()`，比普通窗口更早撞上限 |
| 8 核只起来一部分 | 看 `q8b: PSCI CPU_ON core N failed`；确认 SC8280XP 的 MPIDR（Aff1=core）与 PSCI conduit（SMC） |
| EUD SRST 后串口出现 `05c6:900e`/`Sahara` | 已知现象；**再发一次 reboot** 可回到 UEFI。MCP 现在以串口缓存判定：UEFI/PXE 即成功，Sahara 才重发（最多 2 次）；旧 900e 仅作超时兜底 |

**内存上限**：`bsp/hw_arch.h` 里 `Q8B_USABLE_LIMIT = 8GB`，`kalloc_arch()`
再按 UEFI 内存图只收录可回收区，扣掉内核池后实机可分配约 7.2GB。
若后续换内存配置，确认 `Q8B_KERNEL_PHYS` 仍在可用区间内，并保持
RAM disk / GOP 帧缓冲被排除在分配器之外。
