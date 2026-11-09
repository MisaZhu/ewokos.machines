# Q8B/TW220 调试参考

## 本案已确认的硬件事实

| 项目 | 值 |
|---|---|
| 实际 IC | Goodix Berlin 7986P；ACPI `GXTS7986`；Windows `GTBerlinB` |
| EwokOS 兼容串 | `goodix,gt9916` |
| 总线 | SPI22 / QUP2 SE6，mode 0，8 bit，最大约 3 MHz |
| 引脚 | CLK83、CS84、MISO85、MOSI86；mux 83/84=selector 2，85/86=selector 1 |
| 控制 | reset99 低有效，IRQ175 低有效，VDDIO90、AVDD56 高有效 |
| 共享电源 | PMC8280_1 GPIO1，SID1 peripheral `0x8800`，输出高 |
| 面板 | 2560 × 1600；显示客户端使用 `y_rev=true` |

原始 probe 失败的两个直接原因是共享 VCC3B 未初始化和 bit-bang pin role 错误。修复后应先用 AA echo、固件校验和 IC_INFO 证明 IC 可通讯，再处理事件协议。

## 原生 SPI 资源

- SE 物理基址 `0x00898000`、长度 `0x4000`；wrapper `0x008c0000`。
- GCC：QUP2 vote `0x52010`，SE6 vote `0x52018`；不得覆盖整寄存器或在退出时关闭共享 vote。
- `GENI_IF_DISABLE_RO` 位 0 为 1 时，Linux `spi-geni-qcom` 选择 GPI DMA；不要试写 FIFO capability/config 页面。
- DTS 声明 CX power-domain 和三条 ICC：`qup-core`、`qup-config`、`qup-memory`。对应完整 BCM 路径见主 Skill。
- QUP2 command-db 资源地址 `0x50064`；CN2 `0x50044`；CN0 `0x5003c`；SH2 `0x5000c`；SN3 `0x50018`；SN0 `0x50010`；SH0 `0x50004`；MC0 `0x50000`；ACV `0x500ac`。这些是 command-db 地址，不是当前 vote 值。
- apps RSC drv-2 物理窗口 `0x18220000`，TCS offset `0xd00`，TCS stride `672`；TCS0 是共享资源，必须先确认 control/status/IRQ 空闲并用新鲜 response IRQ 判断完成。

## GPI/SMMU

- GPI2 物理资源 `0x00800000..0x0085ffff`，`qcom,sm6350-gpi-dma` 的 EE offset `0x10000`；instance context stride `0x4000`。
- instance 0 的 channel/event context 大致在 `0x00810000/0x00811000`，命令寄存器在 `0x00813008/0x00813010`；只读检查不能写这些地址。
- DTS DMA cells `<gpi_dma2 0 6 QCOM_GPI_SPI>` 和 `<... 1 6 ...>` 中的 `6` 是 serial engine index。
- 真机只读结果：SID `0xb6`，SMMU `SCR0=0x00210406`，ID0/1/2=`0x4c017ea2/0x6000006e/0x00005111`，无匹配有效 SMR，unmatched policy=fault，CLIENTPD=0。
- GPI 控制命令必须依赖新鲜 IRQ，而不是轮询 CHSTATE；Linux opcode 为 allocate=0、start=1、stop=2、reset=9、dealloc=10。DEALLOC 后硬件状态可能保持 allocated。
- DMA ring、event ring、descriptor 和 payload 必须由内核持有并 pin 到 quiescence；普通进程退出不能让硬件继续访问可复用 buffer。

## 诊断记录规范

推荐每个工具启动时记录：工具哈希、PID、`CNTVCT_EL0`、`CNTFRQ_EL0`、`kernel_tic`、init uuid/start/state。`init uuid` 是进程标识，不是 boot ID；没有跨事件的 uptime/counter 证据时，不能宣称发生过重启。

设备网络栈有约 180 秒 live TCP idle timeout；SSH/SCP 使用 `ServerAliveInterval=15`、`ServerAliveCountMax=4`。把 `/dev/log` 当作会被读取消费的流；关键输出同时保存到设备文件，并在命令结束后另行读取。

## Goodix 事件陷阱

- 零 contact release frame 没有 contact payload checksum，不能用旧 payload 尾部校验拒绝它。
- release 必须保留最后一次 contact 坐标；强制 `(0,0)` 会让 `xtouch` 无法识别点击。
- 修改坐标方向后，单独验证点按、抬起、拖动和按钮执行；“能移动”不等于“能点击”。
