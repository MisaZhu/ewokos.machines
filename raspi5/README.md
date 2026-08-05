# raspi5 — EwokOS Raspberry Pi 5 (BCM2712) 移植

参考 `raspix` 实现的独立 machine 目录。内核已可编译，串口调试输出贯穿
引导关键路径（`[pi5]` 前缀）。用户态（system/）尚未移植。

## 支持硬件

| 机型 | board revision |
|------|----------------|
| Raspberry Pi 5 (2G/4G/8G/16G) | 0xb04170 / 0xc04170 / 0xd04170 / 0xe04171 |
| Compute Module 5 | 0x?03170 |
| Raspberry Pi 500 | 0xc04181 |

注意：EwokOS 为 32 位地址空间，可用内存上限 2GB（多余部分被截断）。
QEMU 暂无 raspi5 机型，仅支持真机启动。

## 构建

```sh
# 需要 aarch64 裸机工具链（arm-gnu-toolchain-*-aarch64-none-elf）
export PATH=$PATH:~/toolchain/arm-gnu-toolchain-14.3.rel1-darwin-arm64-aarch64-none-elf/bin

cd raspi5/kernel
make ewokos=<ewokos 内核源码根目录>    # 默认 ~/work/ewokos
```

产物：`kernel8.img` / `kernel8.elf`。

## SD 卡启动准备

1. 使用官方 Raspberry Pi OS 固件文件（boot 分区）：`bootcode.bin`、
   `start.elf`、`fixup.dat` 等（Pi 5 由 EEPROM bootloader 加载）。
2. 拷贝 `kernel8.img` 到 boot 分区。
3. `config.txt` 至少包含：

```ini
arm_64bit=1
enable_uart=1
uart_2ndstage=0
dtoverlay=vc4-kms-v3d
disable_fw_kms_setup=0
```

`disable_fw_kms_setup=1` must not be used. BCM2712 firmware no longer
implements the legacy mailbox framebuffer allocator; EwokOS adopts the boot
scanout buffer published by the firmware in the `simple-framebuffer` DT node.

（若 EEPROM 配置已允许 `kernel=kernel8.img` 则无需额外指定。）

## 串口调试

- 引脚：GPIO14 (TX) / GPIO15 (RX)，115200 8N1。
- USB-TTL 线：RX↔GPIO14、TX↔GPIO15、GND 共地。
- 内核在以下关键点输出 `[pi5]` 前缀日志（即使 UART 未就绪也有超时保护，不会挂死）：

```
[pi5] _boot_start: page tables ready, MMU enabled
[pi5] sys_info: board=raspberry-pi5-4g
[pi5] uart: PL011 init clk=... baud=... ibrd=... fbrd=...
[pi5] timer: ARM generic timer, cntfrq ok
[pi5] irq: GIC-400 initialized
[pi5] sd: init EMMC host ...
[pi5] sd: card ready, capacity=... MB, high_capacity=1
[pi5] smp: core ready
```

## BCM2712 关键地址（相对 ARM 物理地址）

| 外设 | 物理地址 | 内核虚拟映射 |
|------|----------|--------------|
| 外设窗口 | 0x7C000000 (64MB) | MMIO_BASE |
| UART0 (PL011) | +0x201000 | MMIO_BASE+0x201000 |
| VPU mailbox | +0x13880 | MMIO_BASE+0x13880 |
| GIC-400 GICD/GICC | +0x3FF9000 / +0x3FFA000 | MMIO_BASE+... |
| EMMC (SDHCI) | 0x1000FFF000 | MMIO_BASE+0x4000000 窗口 |
| RP1 南桥 | 0x1F00000000 (32MB 窗口) | MMIO_BASE+0x8000000 |

## 移植现状 / TODO

已完成：
- 引导（boot 页表 → MMU → 内核高位地址）、board 识别（mailbox）
- PL011 串口、ARM 通用定时器（CNTV）、GIC-400 中断/IPI、SMP 四核
- SD 卡启动（SDHCI/EMMC 控制器，复用 raspix MMC 协议栈）

TODO：
- `system/` 用户态移植：RP1 驱动（framebuffer、USB、以太网、GPIO、I2C/SPI、声音）
- Pi5 上 UART 时钟经 mailbox 查询，失败时回退 48MHz
