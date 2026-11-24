# ga10bd 审核调研资料索引

日期：2026-11-22 ~ 23
对象：`system/drivers/ga10bd`（Tegra234 GA10B GPU 3D 驱动，未上过硬件）
方法：4 个 subagent 分块对照 nvgpu 源码逐项核对，主流程再按源码复核后才改代码。

## 文件

| 文件 | 范围 | 条目 |
|---|---|---|
| [01-fifo-runlist-ce.md](01-fifo-runlist-ce.md) | TOP 设备表、runlist、RAMFC、CHRAM、doorbell、CE | F1–F11 |
| [02-gr-fecs-golden-ctx.md](02-gr-fecs-golden-ctx.md) | GR 使能顺序、FECS/GPCCS 启动与方法、golden ctx、bundle | G1–G10 |
| [03-acr-falcon-pmu.md](03-acr-falcon-pmu.md) | GSP 复位、ACR HS 引导、LS blob、WPR、PMU | A1–A12 |
| [04-mm-fb-init-order.md](04-mm-fb-init-order.md) | 上电顺序、FB/MMU、GMMU 页表、TLB、priv ring/bus/MC、时钟 | M1–M9 |

每个文件含两段，均为**原文照录**：下发给 subagent 的任务、subagent 的返回结果。

## 可信度说明

- subagent 结论是**读源码得出的线索**，没有运行任何东西。其中标 `CANNOT-DETERMINE` 的项源码无法判定，需要硬件。
- 下表"处置"列为主流程的后续动作。"已修复"的条目在改代码前都对照源码复核过；"遗留"的条目**未逐条复核**，引用前请重新核对源码。
- 全部验证只到 host dry-run 测试 + aarch64 交叉编译为止。dry-run 下寄存器读回 0、轮询直接成功，只能证明"写了什么、顺序如何"，**不能证明握手在真机上成立**。

## 参考源码（位于 /tmp，会丢失）

| 路径 | 来源 | 版本 |
|---|---|---|
| `/tmp/gpu-research/linux-nvgpu` | github.com/OE4T/linux-nvgpu | 分支 `patches-rel-36`，commit `21d928824dc7ca3dc17603a53b11edc6641ace2d` |
| `/tmp/gpu-research/t23x-public-dts` | github.com/OE4T/t23x-public-dts | tag `jetson_36.5`，commit `ed1b0f6b113bb050c8cda1ccb411a163f8e2799f` |
| `/tmp/gpu-research/headers/` | tegra234 clock/powergate/reset 头、bpmp-abi.h、tegra234.dtsi、nouveau_pci.c | 未记录出处版本 |
| `/tmp/gpu-research/Orin-TRM.pdf`（及 .txt、vic-chapter.txt） | Orin TRM | 未记录版本 |

结果中的 `REF` / `R` 前缀即 `linux-nvgpu/drivers/gpu/nvgpu`；行号对应上面的 commit。

## 处置一览

### 01 FIFO / runlist / CE

| 条目 | subagent 结论 | 处置 |
|---|---|---|
| F1 设备表解析 | 把行数当设备数、固定 3 行步长 | 已修复：按 num_rows + chain 位解析，`ga10b_devtable_parse` |
| F2 runlist 选择 | 取"第一行"而非按设备类型 | 部分修复：GR runlist 取 GRAPHICS 设备。**遗留**：RAMFC target 按 runlist 上最小 engine_id 选取未实现；CE 独立 runlist 未实现 |
| F3 RAMFC | signature 应为 0xC76F、acquire 应为 0x102 | 已修复这两项。**遗留**：intr_notify 向量仍为 0；config/hce 恒按特权通道写；CE 通道 subctx 表未写 |
| F4 CE 子通道 | nvgpu 用 4 | 已修复 |
| F5 TSG timeslice | 应为 0x80030001 | 已修复（数值为 subagent 推导，未在硬件验证） |
| F6 runlist 提交 | 不应写 sched-disable；VIRT_CH_CFG 应初始化时 RMW 一次 | 已修复 |
| F7 CHRAM 顺序与拆除 | 应 ramfc → runlist → enable；拆除缺 clear 与 runlist 移除 | 已修复。**遗留**：TSG disable/preempt 未实现 |
| F8 doorbell | MATCH | 无改动 |
| F9 FIFO 初始化 | runlist CG 写错地址，缺 userd writeback | 已修复这两项。**遗留**：runlist/PBDMA 中断使能未搬（轮询模式下 PBDMA 故障不可见） |
| F10 CE 初始化与流 | 应复位全部 LCE、中断掩码 0x8000007F、notify 对 CPU 关闭 | 已修复这三项。**遗留**：CE BLCG `0x104200=0xc244`、`0x104044=1` 未写；blit 用虚拟地址 + CE 信号量，与 nvgpu 的物理地址流不同，源码无法判定是否可行 |
| F11 USERD/GPFIFO | MATCH | 无改动 |

### 02 GR / FECS / golden ctx

| 条目 | subagent 结论 | 处置 |
|---|---|---|
| G1 顺序 | GR 引擎/GPC 复位应在 ACR 之前，缺 non-ctx 列表 | 已修复：新增 `ga10b_gr_enable_hw`，在 ACR 前执行 |
| G2 falcon 启动 | 写错了 mailbox（应为 ctxsw mailbox 0x409800 系列） | 已修复 |
| G3 FECS 方法 | 各方法的清除位/成功/失败条件各不相同 | 已修复：按方法查表 |
| G4 wait_ctxsw_ready | 缺 current_ctx 失效与 SET_WATCHDOG | 已修复。看门狗真机取值可能由 sysclk 推导，未定 |
| G5 golden 前硬件初始化 | 缺 fifo_access、non-ctx 列表、mem scrubbing、gpc_mmu、早期 sm-id | 已修复这些。**遗留**：GR/FECS 中断与异常使能、GR BLCG、SM-id 表为线性而非 nvgpu 的性能排序 |
| G6 golden 内容 | MIG 与非 MIG 行为混用 | **遗留**：属设计取舍，需硬件验证后定 |
| G7 bundle | stats-counter 掩码不应有 | 已修复（删除掩码） |
| G8 复位值 | MATCH，仅延时应为 20us | 已修复延时 |
| G9 布局 | MATCH | 无改动 |
| G10 通道上下文 | 若干小缺口（0xd8/0xdc、access map、subctx 无效项、mme ptimer） | **遗留** |

### 03 ACR / falcon / PMU

| 条目 | subagent 结论 | 处置 |
|---|---|---|
| A1 GSP 复位 | 应走 0x1103c0 引擎复位 + scrub 等待 | 已修复 |
| A2 A3 A5 A6 A9 A10 A11 | MATCH | 无改动 |
| A4 HS 完成检查 | 缺 PMU BAR0 错误状态、GSP ECC 状态两项诊断 | **遗留**（仅诊断） |
| A7 blob 镜像顺序 | nvgpu 为 GPCCS 在前 | 已修复（ACR 是否在意顺序源码无法判定） |
| A8(ii) next-core `bl_data_*` | 不应清零 | 已修复。**注意**：主流程曾只看静态填充段（:686-691）而否决此条；复核发现之后的 `lsf_calc_wpr_size_pkc`（:935-943）对所有镜像重新赋值，subagent 是对的。当前驱动未使用 next-core 镜像，此路径是潜在问题 |
| A12 PMU 引导 | 现有实现是非安全路径，熔丝片上不可用 | 已修复为拒绝执行。**遗留**：LS-PMU 安全路径未实现 |

### 04 MM / FB / 初始化顺序

| 条目 | subagent 结论 | 处置 |
|---|---|---|
| M1 顺序 | ACR 须在 FB/LTC/mm/fifo/gr_enable_hw 之后；中断屏蔽应最先；不应写 ELPG | 已修复 |
| M2 FB 初始化 | wr/rd 调试缓冲应分页 | 已修复分页。**遗留**：bar1/bar2 bind、fb_flush、FB 中断、mmu_fault 设置 |
| M3 M4 | MATCH | 无改动 |
| M5 TLB 失效 | 多写了 all_pdb，缺前后等待 | 已修复 |
| M6 priv ring/bus/MC | 基本 MATCH | 仅调整顺序。bus `0x1100` W1C 写保留（nvgpu 无此写，影响未定） |
| M7 引擎复位 | MATCH，应拒绝 reset_id ≥ 32 | 已修复 |
| M8 芯片识别 | 掩码应为 0x1FF | 已修复掩码。未单独判断 `boot_0 == 0xFFFFFFFF`：掩码后为 0x1FF ≠ 0x17B，已被 ID 比较拒绝 |
| M9 时钟 | nvgpu 对各时钟设最大频率 | **遗留**：未设频率 |

## 与既有记录的冲突

项目记忆中"RAMFC sig=0xface（非 0xC76F）"与源码相反：ga10b 接线的是 `gp10b_pbdma_get_signature`，值为 0xC76F（F3）。记忆文件未改动。
