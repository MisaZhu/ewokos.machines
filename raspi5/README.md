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
disable_fw_kms_setup=0
hdmi_force_hotplug=1
dtoverlay=vc4-kms-v3d
```

关于 framebuffer：

- `disable_fw_kms_setup=1` 不能使用。BCM2712 固件不再实现 mailbox 传统
  framebuffer 分配器（`ALLOCATE_BUFFER` 只会把固件启动时已经在扫描输出的那块
  buffer 交回来），所以 EwokOS 只能沿用固件建立的 boot scanout buffer。
- `dtoverlay=vc4-kms-v3d` 需要保留。虽然 EwokOS 没有 DRM/KMS 驱动，但 Pi 5 的
  HDMI 时钟树是由该 overlay 描述的；实测去掉之后固件不点亮 HDMI，`fbd` 拿到的
  buffer 无处输出，屏幕全黑。
- 分辨率由 config.txt 决定。要指定模式请用带端口下标的形式（Pi 5 有两个 HDMI 口），
  例如 480x800@60：

```ini
hdmi0_group=2
hdmi0_mode=87
hdmi0_cvt=480 800 60 6 0 0 0
```

  不带下标的 `hdmi_group` / `hdmi_mode` 是 VC4（Pi 4 及更早）的遗留写法，在 Pi 5
  上不生效。不写任何 `hdmi*` 时，固件自带的 HDMI 初始化只输出 VGA / 720p / 1080p。
- `etc/gui/framebuffer.json` 里的宽高在 Pi 5 上被忽略。`fbd` 改为用 mailbox 的
  `GET_DISPLAY_DIMENSIONS` 问固件当前显示尺寸，把 framebuffer 建成和屏幕一样大，
  避开了“config.txt 写 480x800、json 写 800x600”两头不一致的坑。
- 像素格式：`SET_DEPTH`=32 本身就意味着 ARGB8888，不需要再发 `SET_PIXEL_ORDER` 或
  `SET_ALPHA_MODE`（Circle 在所有 Pi 上都不发这两个 tag），因此刷屏就是纯 `memcpy`。
- 驱动初始化顺序参照 [rsta2/circle](https://github.com/rsta2/circle)
  `lib/bcmframebuffer.cpp`：

  1. `GET_NUM_DISPLAYS` (0x00040013)
  2. **`SET_DISPLAY_NUM` (0x00048013) 选定显示器 0**——Pi 4/5 有两个 HDMI 口，
     不先选定的话后面所有 framebuffer tag 作用在哪个口上是不确定的
  3. 单条消息里依次 `SET_PHYS_WIDTH_HEIGHT` / `SET_VIRT_WIDTH_HEIGHT` / `SET_DEPTH` /
     `SET_VIRTUAL_OFFSET`（0,0，否则可能沿用启动时的旧偏移）/ `ALLOCATE_BUFFER` /
     `GET_PITCH`
  4. 上面任一项返回 0 则整次失败，此时回退到“只采纳固件已有 buffer”

  串口上的 `fb_init:` 一行会打印最终的分辨率、`pitch`、`phy=`、`order=` 和
  `displays=`。

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
| 外设窗口 | 0x10_7C000000 (64MB) | MMIO_BASE |
| UART0 (PL011) | +0x1001000 | MMIO_BASE+0x1001000 |
| VPU mailbox | +0x13880 | MMIO_BASE+0x13880 |
| GIC-400 GICD/GICC | +0x3FF9000 / +0x3FFA000 | MMIO_BASE+... |
| EMMC (SDHCI) | 0x1000FFF000 | MMIO_BASE+0x4000000 窗口 |
| RP1 南桥 | 0x1F00000000 (32MB 窗口) | MMIO_BASE+0x8000000 |
| 固件 framebuffer 保留区 | 0x3C000000 (64MB) | 由 fbd 按需 mem_map |

VPU 只能通过 `0xC0000000` 这一个 RAM 别名看到 ARM 物理 0..1GB
（官方 `bcm2712.dtsi`：`dma-ranges = <0xc0000000 0x00 0x00000000 0x40000000>`），
所以 mailbox 请求缓冲区和固件 framebuffer 都必须落在低 1GB 内。

## 移植现状 / TODO

已完成：
- 引导（boot 页表 → MMU → 内核高位地址）、board 识别（mailbox）
- PL011 串口、ARM 通用定时器（CNTV）、GIC-400 中断/IPI、SMP 四核
- SD 卡启动（SDHCI/EMMC 控制器，复用 raspix MMC 协议栈）

TODO：
- `system/` 用户态移植：RP1 驱动（framebuffer、USB、以太网、GPIO、I2C/SPI、声音）
- Pi5 上 UART 时钟经 mailbox 查询，失败时回退 48MHz
