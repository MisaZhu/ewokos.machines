---
name: q8b-tw220-spi-bringup
description: "排查 Q8B/TW220 Goodix 触摸失效、GPIO SPI 到原生 GENI/GPI SPI 迁移，以及无串口真机调试时使用；适用于 DTS、ACPI、Windows PEP 与 EwokOS 驱动需要交叉核对的场景。"
---

# Q8B/TW220 触摸与 SPI bring-up

先保持已验证的 GPIO 触摸路径可回滚，再逐层验证原生 SPI。这个 Skill 记录的是决策依据和危险边界，不授权部署、重启或改写共享硬件资源；这些动作仍需当前任务的明确授权。

## 工作顺序

1. **静态收集**：读取项目 DTS、驱动、`init.rd`、板级 BSP 和上游 Linux 对应驱动；同时查 TW220 项目的 ACPI/PEP 资源和 Windows 硬件 ID。不要根据 DTS 节点名猜测实际设备。
2. **建立硬件事实表**：记录实际 IC、总线、引脚复用、SPI 模式/频率、IRQ/reset 极性、电源轨、共享电源、坐标方向。用寄存器回读、Goodix 回显、固件/IC_INFO 读取证明每一项。
3. **先修复 GPIO 基线**：电源和复位必须有顺序、延时、屏障和写后读回；保留稳定驱动的哈希、备份和 `init.rd`。触摸协议验证要覆盖按下、移动、抬起、空接触帧、校验和及坐标方向。
4. **只读真机诊断**：诊断程序必须有阶段 checkpoint、本地日志、超时和明确的退出状态。SSH 使用保活；空闲 TCP 断开、sshd banner 超时、旧 `/dev/log` 内容和 PID 变化都不能单独证明重启、总线卡死或某个 MMIO 访问导致故障。
5. **原生 SPI 分层验证**：依次验证 GCC/QUP 时钟、GENI 版本和固件、FIFO/GPI 能力、pinmux、CX 电源域、ICC/NoC 路径、RPMh 资源、GPI 通道和 SMMU 路由。任何一层失败都停止向下一层写寄存器。
6. **迁移前提**：当 `GENI_IF_DISABLE_RO` 表明 FIFO 不可用时，走 GPI DMA；不要强行写受保护的 `SE+0x2000..0x2008`。先取得空闲 GPI 实例、建立 DMA ring 生命周期和 SID 映射，再提交 CONFIG/START/DMA 描述符。

## 关键判断

- TW220 实际触摸 IC 可由 ACPI `GXTS7986`、Windows `GTBerlinB`、Goodix Berlin `7986P` 相互印证；DTS 的 `goodix,gt9916` 是兼容驱动标识，不是实际料号。
- 已验证硬件连接：SPI22/QUP2 SE6，CLK83、CS84、MISO85、MOSI86，mode 0、8 bit、≤3 MHz；reset99 低有效，IRQ175 低有效，VDDIO90、AVDD56 高有效，另需 PMC8280_1 GPIO1 的共享 VCC3B。DTS 注释中的 pin role 不能替代回读证据。
- 原始“无响应”优先检查共享 VCC3B 和 pin role；本案两者缺失/错误即可解释 Goodix 不回包。
- SPI 原生链路的完整资源路径为：core `QUP2`；config `SH2 -> CN0 -> CN2`；memory `SN3 -> SN0 -> SH0 -> MC0 + ACV`。只准备 GCC 时钟不等于 SPI 可用。
- GPI DTS 的 DMA cell 中的 `6` 是 SE index，不是 GPI instance。GPI2 SID `0xb6` 没有有效 SMMU SMR 且 unmatched policy 为 fault 时，DMA 必然不能工作；CPU MMIO 诊断不能因此推断成 DMA 故障。
- GPI `DEALLOC` 不保证 CHSTATE 变回零；必须等待新的控制 IRQ。禁止用 CHSTATE alone 判断所有权、复用已超时实例或清除其他通道的 IRQ。
- RPMh TCS 空闲不等于 BCM 资源空闲。共享投票必须保留已知 baseline，平均带宽累加、peak/corner 取最大、enable mask 做 OR，并按 VCD 分组、最后一项提交；提交超时后冻结状态，禁止盲目重试或恢复寄存器。

## 代码与验证要求

- 开发诊断工具和正式 `goodixd` 分开；诊断工具不能进入 `init.rd` 自启动。
- 本地日志必须在每条记录后关闭或执行真实 flush；EwokOS `fflush()` 是 no-op，不能把“调用 fflush”当作落盘证明。
- 每个硬件写入前打印阶段并留有安全退出点；不做无依据的 reset、全局 SMMU bypass、共享 TCS 覆盖或 pinmux 夺取。
- 主机测试至少覆盖 SPI 分帧、GPI 命令超时/陈旧 IRQ/错误状态/有序释放、command-db 边界和共享 RPMh 投票合并；通过主机测试不等于硬件验证。
- 真机验证顺序应是只读 runtime、资源表、SMMU 路由、时钟，再考虑单个可回滚的硬件动作。失败时保留设备日志、工具哈希、正式驱动哈希和进程状态。
- 迁移完成前，`init.rd` 保持 GPIO 正式驱动；只有原生 SPI 完成端到端 Goodix 探测、连续读写、触摸事件和重启验证后才允许替换。

需要寄存器偏移、资源表字段、Linux 对照位置和本案测量值时，读取 [references/debug-playbook.md](references/debug-playbook.md)。
