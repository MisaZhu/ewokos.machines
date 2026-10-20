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
# 保存真 kernel
cp .../KERNEL.BIN .../KERNEL_ENTRY.BIN
# 部署 8 字节 halt: wfi; b __entry @0x90080000
cp machines/q8b/test/halt_kernel.bin .../KERNEL.BIN
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
cp .../KERNEL_ENTRY.BIN .../KERNEL.BIN
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
make efi KERNEL_ONLY=1 FORCE_UART17=1
cp boot/uefi/BOOTAA64.EFI .../pxe/tftp/q8b/
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
- 测试期间不要跑 `board_cycle.sh`/`make deploy`，它们会覆盖 `KERNEL.BIN`。
- `[89C` 不是 kernel；它是 UEFI console 的 ANSI，常出现在 PXE 之前。
- 不要假设 BIOS 的 UART17 初始化跨 EBS 保留；EBS 后 loader 裸写也不出字，
  kernel 需要自己初始化 QUP2/SE1/GCC（或让 UART 在 EBS 后保持可用）。
- kernel 早期 TX 必须按 GENI 顺序：FIFO → TX_TRANS_LEN → M_CMD0 → 等 done，
  不能先等 done 再写 FIFO。
