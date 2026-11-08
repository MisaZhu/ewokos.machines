---
name: q8b-kernel-test
description: EwokOS Q8B 内核标准测试流程：MCP 常驻串口缓存、reboot -> PXE -> cap read、900e/EDL 判定、halt kernel 隔离、恢复真 kernel、内核第一个串口字符。
---

# Q8B 内核标准测试流程

## 前提

- MCP 前台/socket 运行；串口常驻监控固定 `/dev/tty.usbserial-0035E5970`，
  输入带时间戳写入 `eud/logs/serial-cache.log`。
- PXE：`en6`/`192.168.8.1`、dnsmasq、TFTP `q8b/BOOTAA64.EFI`（KERNEL_ONLY）。
- EUD 9501 在位；板子原生 Type-C；40-pin USB-UART 已接。
- 读取一律走缓存，不直接读 tty。
- MCP 串口执行系统命令前，先确认已经进入 root shell；登录用户名 `root`、
  密码 `root`。已有 root shell 时不重复登录，不向 UEFI 菜单发送 shell 命令。
- SSH 连接失败时，先通过已登录的 MCP 串口执行 `/bin/ps`，确认 `/sbin/sshd`
  是否运行，不直接认定整板挂死。复位后等待启动完成，不叠加复位。

## 部署启动载荷（原 deploy.sh）

`machines/q8b/deploy.sh` 已移除；部署按本节手工执行。下文的 `TFTP`/`.../`
均指 PXE/TFTP 投放区 `/Users/tony/work/q8b/boot/tftp/q8b`（ewokos 旁边的
`q8b/` 工作树，由 `q8b/boot/start.sh` 提供 TFTP 服务）。

### 构建 EFI 载荷

```bash
make -C machines/q8b/kernel      # 内核 -> machines/q8b/kernel/kernel.bin
make -C machines/q8b/system      # 系统 -> machines/q8b/system/build_aarch64
make -C machines/q8b/efi         # 打包 -> efi/kernel.bin、efi/BOOTAA64.EFI、efi/initramfs.img
```

缺 `efi/` 三个产物时先跑 `make -C machines/q8b/efi`（旧 deploy.sh 同样会校验
这三个文件后报错）。

### 完整投放（PXE/TFTP）

```bash
TFTP=/Users/tony/work/q8b/boot/tftp/q8b
mkdir -p "$TFTP"
cp machines/q8b/efi/BOOTAA64.EFI  "$TFTP/BOOTAA64.EFI"
cp machines/q8b/efi/kernel.bin    "$TFTP/KERNEL.BIN"
cp machines/q8b/efi/initramfs.img "$TFTP/INITRAMFS.IMG"
rm -f "$TFTP/ROOTFS.IMG"        # SD_ROOTFS=1（默认）: rootfs 留在 SD ext3 分区
# SD_ROOTFS=0: cp machines/q8b/root_aarch64.img "$TFTP/ROOTFS.IMG"
```

EUD 开发服务有独立 TFTP 树（`/Users/tony/work/q8b/eud/pxe/tftp/q8b`），把同样
三个文件镜像过去，保持两种 PXE 启动方式同步；不需要就跳过。

### 只换 KERNEL.BIN（halt 测试 / 恢复真内核）

```bash
TFTP=/Users/tony/work/q8b/boot/tftp/q8b
cp machines/q8b/test/halt_kernel.bin "$TFTP/KERNEL.BIN"  # 8B halt: wfi; b __entry @0x90080000
cp "$TFTP/KERNEL_ENTRY.BIN" "$TFTP/KERNEL.BIN"           # 恢复真 kernel
```

### 启动方式与串口/BIOS

- PXE：载荷放到 `q8b/boot/start.sh` 服务的 TFTP 根，DHCP bootfile 指向
  `q8b/BOOTAA64.EFI`。**直接 PXE NBP 最可靠**（保留固件网络 LoadFile
  handle）；GRUB chainload 可能可用。
- SD_ROOTFS=1 时 loader 经 `EFI_LOAD_FILE`（TFTP）读 KERNEL.BIN/
  INITRAMFS.IMG；持久 rootfs 由 init 切换后从 SD ext3 分区读取。
- 串口：40-pin Pin6=GND、Pin8=TXD、Pin10=RXD，115200 8N1（**不要接 VCC**）。
- BIOS：F2 → Debugging Settings → Synchronous Debug UART in UEFI = Enabled，
  可看到 UEFI 阶段日志。

## 0. 检查

```text
eud> hw
eud> cap status
eud> pxe
```

MCP：

```json
{"name":"eud_hw_status","arguments":{}}
{"name":"eud_serial_log","arguments":{"action":"status"}}
{"name":"pxe_status","arguments":{}}
```

期望：9501 是；缓存监控运行；PXE 就绪。

## 1. halt kernel 隔离（最小验证）

```bash
# TFTP 投放区见上文『部署启动载荷』节
TFTP=/Users/tony/work/q8b/boot/tftp/q8b
# 保存真 kernel
cp "$TFTP/KERNEL.BIN" "$TFTP/KERNEL_ENTRY.BIN"
# 部署 8 字节 halt: wfi; b __entry @0x90080000
cp machines/q8b/test/halt_kernel.bin "$TFTP/KERNEL.BIN"
```

执行 `reboot`/`boot`，预期串口：

```text
[uefi] KERNEL_ONLY: skipping ROOTFS.IMG
[uefi]   MTFTP4 ok, 8 bytes
Exit EBS ... UEFI End
```

此后无内核输出；板子停住 = loader/EBS 正常。若仍复位/900e，问题在
stub/EBS/PMIC/WDT，不在 kernel 代码。

## 2. 标准 reboot -> PXE -> cap read

1. 记录 `T0 = YYYY-MM-DD HH:MM:SS`。
2. `eud_board_reboot {"method":"eud"}` / 控制台 `reboot`。
3. helper 等待 PXE/loader：出现 `Start PXE` / `KERNEL_ONLY` / `MTFTP4` /
   `Exit EBS` 即成功；出现 `Sahara` 才再次 SRST；旧 900e 只作超时兜底。
4. 读缓存：

```text
eud> cap read                 # 默认最后 50 行
eud> cap read since=2026-10-18T20:25:23
```

或 MCP：

```json
{"name":"eud_serial_log","arguments":{"action":"read"}}
{"name":"eud_serial_log","arguments":{"action":"read","since":"2026-10-18 20:25:23","until":"2026-10-18 20:30:00"}}
```

## 3. 恢复真 kernel

```bash
TFTP=/Users/tony/work/q8b/boot/tftp/q8b   # 见『部署启动载荷』节
cp "$TFTP/KERNEL_ENTRY.BIN" "$TFTP/KERNEL.BIN"
```

## 4. 内核第一个串口字符

目标顺序：

1. `K`：`__entry` 第一条 `q8b_early_mark`（UART17 `0x884000` GENI 直写）；
2. `1`：EL1；
3. `2`：MMU/cache 后；
4. `3`：`_boot_start` 前；
5. `q8b: _boot_start` 等 C 输出。

判定：

- 实测：0x884000 不是裸 UART，而是 `qup2` 的 **SE1**（DTS:
  `qup2: geniqup@8c0000`，`uart17: serial@884000`，时钟
  `GCC_QUPV3_WRAP2_S1_CLK` + QUP2 M/S AHB，`qcom,geni-debug-uart`）。
  只写 SE 寄存器时 `FORCE_UART17=1` 的 loader 自己都没有 `[uefi]` 日志；
  EFI Serial I/O 可用但 EBS 后 boot service 失效。kernel 首字符必须补
  QUP2/GCC/TLMM（pinmux）初始化，或让 UART 在 EBS 后保持可用。
- 看到 `K`/`q8b:` → 内核入口和 UART 都通了；
- loader 有日志、kernel 无任何字符 → 优先查 EBS 后 UART17 的 QUP 时钟/
  pinmux/SE 状态，以及 loader 的 EFI Serial I/O 是否在 EBS 停掉了 UART；
- 可先用 `FORCE_UART17=1` 重编 loader，排除 EFI Serial I/O 影响：

```bash
cd machines/q8b/kernel
make -C ../efi efi KERNEL_ONLY=1 FORCE_UART17=1
cp ../efi/BOOTAA64.EFI "$TFTP/"   # $TFTP 见『部署启动载荷』节
```

- `KERNEL_ONLY` 下 kernel 会在 `q8b: bootinfo` 后停住；要跑完整 init 需
  恢复 rootfs/真 kernel。

### 4.1 EBS 后 UART 实测补充

- 在 loader 的 `ExitBootServices` 之后、跳 kernel 之前，用裸 UART17 写
  `post-EBS` 标记：**不出现**。说明 BIOS 的 UART17 初始化不会跨 EBS 保留，
  UEFI serial driver 在 EBS 停掉了 UART。
- `[89C` + 空格是 **PXE 之前的 UEFI console 残留**，不是 kernel 输出。
  判定 kernel 必须先看到 `MTFTP4` / `Exit EBS`，并且只看这些行之后的内容。
- kernel 侧 `q8b_early_uart_init()` 的实测寄存器：
  - GCC：`0x152010` bit1(S AHB)/bit2(M AHB)/bit5(SE1)；
    poll `0x11e008`/`0x11e004`/`0x11e274`。
  - GENI：`GENI_OUTPUT_CTRL=0x24`、`GENI_FORCE_DEFAULT=0x20`、
    `SE_GENI_CGC_CTRL=0x28`、`SE_DMA_GENERAL_CFG=0xe30`、
    `SE_IRQ_EN=0xe1c`、`SE_GENI_DMA_MODE_EN=0x258`、
    `SE_GSI_EVENT_EN=0xe18`。
  - packing 8bit/4word/lsb：`0x4380e/0xc3e0e`。
  - TX 顺序：FIFO `0x700` → `TX_TRANS_LEN 0x270` →
    `M_CMD0 0x600` → 等 `0x610 bit30`。
  - 诊断：函数入口在 HDMI `(400,0)` 画红，到达 TX 画绿。
- 当前状态：PXE 到 `Exit EBS` 正常，但 EBS 后仍无 `K/q8b:`。
  若 HDMI 红→绿：查 TX 字节/波特率/packing；若红→无绿：查 QUP2/SE1 初始化；
  若无红：查 `__el1_entry` 到 `q8b_early_uart_init` 的调用链。

## 5. 常见坑

- 旧 `900e` 设备未消失时不要立即判失败；以串口 `Sahara` 为 EDL 依据。
- 真机 UEFI ConOut 与 serial 同一 UART；loader 真机只写 serial，避免重复。
- `KERNEL.BIN` 是 halt 镜像时没有 kernel 输出是预期，不是失败。
- 测试期间不要跑 `board_cycle.sh` 或重新做完整部署，它们会覆盖
  `KERNEL.BIN`；只按『部署启动载荷』节的单文件替换命令覆盖
  （§1 halt / §3 恢复 已用）。
- `[89C` 不是 kernel；它是 UEFI console 的 ANSI，常出现在 PXE 之前。
- 不要假设 BIOS 的 UART17 初始化跨 EBS 保留；EBS 后 loader 裸写也不出字，
  kernel 需要自己初始化 QUP2/SE1/GCC（或让 UART 在 EBS 后保持可用）。
- kernel 早期 TX 必须按 GENI 顺序：FIFO → TX_TRANS_LEN → M_CMD0 → 等 done，
  不能先等 done 再写 FIFO。
- 板子跑飞/挂死/进 EDL 时，**直接再发一次 MCP `eud_board_reboot` 即可恢复**
  （不需要手动断电）。SRST 报“仍检测到 900e/Sahara”或“未确认 UEFI/PXE”时
  不要停，继续重发；EDL guard 会自动再拉，通常 1–2 次后 UEFI/PXE 就出现。
  实测：CM3 release 挂死 → 900E → 重发 reboot → 30–60s 后 EwokOS 起来。

## 6. EwokOS 用户态 GPU 驱动（g2dd）测试流程（已验证）

EwokOS 起来后测 GPU 驱动。g2dd **不自动启动**，每次手工拉起。

### 构建

```bash
make -C machines/q8b/system gui
# 产物: machines/q8b/system/build_aarch64/drivers/q8b/g2dd
```

### 每次测试必须冷启动

上一轮留下的 g2dd 会让下一次启动读到旧 GMU 状态并卡死板子。**每轮测试前先
MCP `eud_board_reboot`，等 ping 通再跑。**

### 部署（ssh，带重试）

```bash
python3 .tmp/mcp_call.py eud_board_reboot '{"method":"eud","wait":80}'
# 200KB 二进制用限速 scp（-l 50 较稳）
scp -O -l 50 g2dd root@192.168.8.50:/drivers/q8b/g2dd
ssh root@192.168.8.50 /bin/md5 /drivers/q8b/g2dd   # 与本地 md5 比对
```

推荐 `deploy_retry.sh`：复位 → scp → md5，不一致/超时就重试（最多 5 次）。
小配置文件直接普通 scp 即可。

### 测试开关（/tmp/adreno.cfg）

`NAME=1` 行表示布尔开关；`NAME=<数字>` 由 `adreno_cfg_int()` 读整数。
改完 cfg 后 scp 到 `/tmp/adreno.cfg` 再启动 g2dd。

### 串口启动 + 抓日志

```text
# 串口登录顺序（每步之间留间隔）
\r → root\r → root\r → \n
# 记录 T0，再启动
/bin/ipcserv /drivers/q8b/g2dd /dev/g2d\n
# MCP 抓 T0 之后的日志
{"action":"read","since":"YYYY-MM-DD HH:MM:SS"}
```

### 验收点（通过则链路正常）

- 串口出现 `dev: /drivers/q8b/g2dd` 与 `q8b g3d: ...` 驱动日志；
- 日志带时间戳，可从 MCP 缓存完整回读；
- 安全测试结束后板子仍能 ping 通（串口读回 `[OK]`）；
- 挂死时 MCP `eud_board_reboot` 重发 1–2 次即可恢复。
