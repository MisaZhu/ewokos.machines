# Orin Nano GA10B GPU 3D 加速 — 移交文档 (2026-11-24)

> **状态 (2026-11-24 18:21): 3D 加速 API 打通, 9 个固定几何核全部移植 —— `cube` / `teapot` / `3ddemo` / `matrix` 四个真实 GLES 应用在 X 桌面上同时经 GA10B 渲染, 连续约 2.5 分钟无超时/报错 (串口 profile 已测; 画面待目视确认)。**
> 端到端自测 `g3dtest`: 7 个场景 (每类核一个) 与 CPU 参考逐像素比对全 PASS, 640x480 / 2012 三角形 0.8–1.3 ms/帧; 4 个实例并发也全 PASS (已测)。
> 3D API 的结构、数据与遗留项见 **§9**; M1 compute 首次跑通的记录见 §2.6。
> **路径变更**: 驱动源码已从 `system/drivers/ga10bd/` 搬到 **`system/libs/ga10b/`** (库 libga10b.a); `drivers/ga10bd/` 只剩 CLI `ga10bd.c`。下文 §5/§6 里的旧路径以此为准。
> 背景/可行性/nvgpu 架构分析见 [gpu-accel-research.md](gpu-accel-research.md);
> 逐模块 nvgpu 对照审计见 [ga10bd-audit/](ga10bd-audit/); VIC 2D 见 [vic-2d-handover.md](vic-2d-handover.md)。
>
> 标注约定: **已测** = 真板实测; **源码** = nvgpu rel-36.4 开源代码核对; **推断** = 未验证推理。
>
> **接手者注意: §3 的"已排除"项都是板上复测过的, 不要重复尝试。§2.4/§2.5 是两个已解卡点的根因, 先读。**

---

## 0. 一句话现状

power→detect→fuse→ACR-GSP HS boot/WPR1→GMMU→FIFO/runlist→PBDMA→**CE blit PASS** (M0) →
PMU RTOS 安全启动 → FBQ RPC (INIT_WPR_REGION + BOOTSTRAP_GR_FALCONS, flcn_status=0) → FECS/GPCCS HS 签名 →
FECS ctxsw INIT_DONE → golden ctx 保存 → compute 通道 (通道 ctx 经 patch buffer 首次被 FECS restore) →
**C7C0 QMD launch 真 sm_87 SASS → `vadd PASS (1024 elements, 4 blocks x 256 threads)`** (M1, **已测**)。

---

## 1. 已测通过的链路 (M0)

板上 `ga --hw ce` (alias `/bin/ga`) 全链 PASS:

| 阶段 | 实测值 |
|---|---|
| power | BPMP pg35 + clks + reset19, GPU BAR0 = 0x17000000 + 16MB |
| detect | chip_id = **0x17b** (GA10B), priv_sec_en fuse = **1** (零售熔丝, 必须走 ACR 安全链) |
| fuse/topology | opt_gpc_disable=2 → **1 GPC / 4 TPC / 8 SM** |
| ACR-GSP | acr-gsp 三件套 HS boot 成功, **WPR1 = 0x277000000 + 0x7e0000**; 现 3-image: PMU(13)lazy=0 + GPCCS(3)/FECS(2)lazy=1 |
| acr_done 判据 | PMU BROM pass (0x10b65c&3==3) + priv lockdown 释放 (0x10a0f4 bit13==0); **一次 power cycle 内 one-shot**, 重跑走 reuse 路径 |
| GMMU | ver2 五级页表, map/dump 正常 |
| FIFO | 多 runlist: gr0@0xc00000, LCE2@0xc00400, LCE3@0xc00800; PBDMA stride 0x800 |
| **CE blit** | 256x64 pitch blit, `memcmp(src,dst)` 一致 → **"CE blit PASS"** |

CE 不依赖 GR falcons / PMU RTOS。**但 ACR 省不掉** (priv_sec_en=1)。

## 1.1 固件与 blob (已测 + 源码)

- 固件目录 **`/Users/tony/work/gpu-fw/ga10b`** (17 文件, 从 L4T r36.4 `nvidia-l4t-firmware` deb 提取)。**NVIDIA 专有, 绝不入 git**; efi Makefile 仅在目录存在时打包进 ramfs `/fs/nvidia/ga10b`。
- LSB header 用 **v1 (2296B)**: ga10b FuSa 构建才用 v2; 零售 L4T = NON_FUSA → v1。**已测**: FECS DMEM 构建串含 `**NO-SAFETY**` 标记, 与 v1 选择互证。
- NETC netlist = `NETC_img_prod_encrypted.bin` (源码: netlist_ga10b.h:45)。**"encrypted" 是误称**, netlist 实为明文 region 表, host 直接解析 (已测 hexdump)。真加密的只有 fecs/gpccs/acr/pmu blob (片上还原验签, 驱动只搬运)。
- nvgpu 源码 (VM 持久): OrbStack debian `~/nvgpu-r36.4-src/linux-nvgpu-l4t-l4t-r36.4/drivers/gpu/nvgpu` (与固件 36.4.7 匹配)。

---

## 2. 卡点证据链与根因 (0x98 → PMU FBQ → golden ctx → compute launch, 均已解)

### 2.1 0x98 已解 (历史, 2026-11-24 早)

旧路线 ACR 直接 bootstrap FECS/GPCCS (lazy=0) → falcon app ~80% 处 halt, ctxsw mbox6=0x98。**根因 (已测+源码)**: FECS/GPCCS app 首处调用是 HS 签名校验 (falcon io 0x280/0x284 期望 0xaf88, 0x288/0x28c 期望 0x8000ab8f/0x8000abcf), 签名由 **PMU bootstrap FECS/GPCCS 时写入**; 旧路线无 PMU 故签名缺失 → 0x98。io 映射 = (GR_addr−0x409000)<<6 (真目标 GR 0x409280-28c)。签名寄存器 HS-priv 保护, host 写不进。
**修复 (已测)**: ACR blob 加 PMU next-core image (lazy=0), FECS/GPCCS 改 lazy=1 (由 PMU bootstrap)。`ga --hw pmu` → PMU RTOS boot OK。详见 memory project-orin-gpu-research「0x98 根因定位」「PMU 安全启动通过」。

### 2.2 PMU FBQ 现象 (已测)

`ga --hw pmu`: stage_acr OK → PMU RTOS boot OK → INIT 消息 (MSG queue) → host 解析 lpq_phy=1/sw_off=0/sw_size=2048 → 写 msgq_tail=1 ack → push RPC (INIT_WPR_REGION) 到 LPQ:

```
cmd head1 (0x10a804) = 1   (host 写了)
cmd tail1 (0x10a824) = 0   (RTOS 永不消费)
msg head/tail = 1/1        (INIT 已 ack, 无后续回包)
RISC-V irqmask (0x10b528) = 0x018428D0   (bit22 FBQ 未 arm)
RISC-V irqsset (0x10b520) = 0            (head 写后无任何中断 pending)
falcon irqstat (0x10a008) = 0xd0         (halt|swgen0|swgen1 恒 pending)
coredump 全 0 (无崩溃), trace buffer 全 0, ssmd 全 0
```

### 2.3 L4T 工作态对比 (已测, 决定性)

L4T 同板 (JP6.2.1 SD, initramfs bash + 固件预置 + chroot modprobe nvgpu + echo on power/control) GPU boot 后:

```
RISC-V irqmask = 0x01C428D0   (bit22 FBQ 已 arm)
cmd head1/tail1 = 0xB/0xB     (消费 11 条 RPC)
msg head/tail = 0xD/0xD
falcon irqstat = 0x0          (RTOS 已服务清中断)
FECS sig 0x17409280 = 0xAF88  (已签名)
```

**唯一动态差异 = RISC-V irqmask bit22 + falcon irqstat**。所有静态输入一致 (§3)。

### 2.4 PMU FBQ 卡点根因 = RTOS dmesg 环无人服务 (已测二分, 2026-11-24 下午)

**根因**: PMU RTOS 的调试打印是**阻塞式**的。RTOS 把日志写进 DMEM 末尾的环 (元数据 {read,write,size,magic=0xf007ba11} 在 DMEM 最后 16 字节), 把写偏移发布到 `queue_head(7)` (0x10a81c), 置 swgen1, 然后**等 host 把读偏移写回 `queue_tail(7)` (0x10a83c)**。nvgpu 的 Linux 构建带 `CONFIG_NVGPU_FALCON_DEBUG=y` (Makefile.linux.configs:24), ISR 里 `ga10b_pmu_handle_swgen1_irq → nvgpu_falcon_dbg_buf_display` 会读环并回写读偏移; ga10bd 是纯轮询, 从不服务 → RTOS 卡在 `initPmuApp` 这条打印的 flush 上, 走不到 FBQ 初始化 (所以 irqmask bit22 不 arm, cmd_tail 恒 0)。

**证据 (三次 fresh boot, 只差一个变量)**:

| 运行 | host 行为 | 结果 |
|---|---|---|
| `--skip 80` | 不清中断、不 ack dmesg (旧行为) | cmd_tail=0, RPC 超时; 只读 dump 出环内容: `ELF-in-place enabled…` / `***Initializing PMU application***` / `initPmuApp` (wr=0x10f, 就停在这) |
| `--skip 100` | 只写 IRQSCLR, 不 ack dmesg | **仍然** cmd_tail=0, RPC 超时, irqmask 仍 0x018428d0 |
| 无 skip | IRQSCLR + 读环 + 写 queue_tail(7) | INIT_WPR_REGION OK, BOOTSTRAP_GR_FALCONS OK, FECS sig=0xaf88 |

所以**必要条件是 dmesg 读偏移 ack**, 不是清中断。旧结论"RTOS 不 arm bit22 / 停 WFI"是现象, 不是原因; "trace buffer 全 0 正常"也成立 (日志走的是 DMEM 环, 不是 cmdline trace_buf)。

**修复** (`ga10b_pmu.c`): `pmu_irq_service()` 仿 `gk20a_pmu_isr`+`gk20a_pmu_handle_interrupts`, 在 INIT 等待 / INIT ack 后 / RPC 回包等待三处轮询里调用: 读 falcon IRQSTAT & (PRISCV IRQMASK & IRQDEST) → 写 IRQSCLR → swgen1 时 `pmu_dmesg_drain()` (DMEMC(0)=off|aincr 读 DMEMD(0), 逐行 klog 为 `ga10b: PMU> …`, 回写 queue_tail(7))。priv lockdown 期间 (BOOTSTRAP_GR_FALCONS 执行中 IRQ 寄存器读回 0xbadf5620) 跳过, 对应 `ga10b_pmu_is_interrupted`。
DMEM 实测: hwcfg dmem=0x10000, 环 off=0xfc00 size=0x3f0。

### 2.5 golden ctx 卡点根因 = 非 MIG 路径缺图形全局 ctx buffer (已测, 2026-11-24 下午)

INIT_DONE 之后 golden init 卡在 `sw_bundle_init[934/935] addr=0x1e100` (最后一条 GO_IDLE bundle), gr_status=0x01200903 永不 idle, 无任何 intr/exception。
**根因**: 驱动此前是 MIG/非 MIG 混合体 (审计 G6 已点名, 当时标"遗留")。sw bundle 要经 **bundle cb** 流过 pipe, 默认 (非 MIG) nvgpu 在 bundle 之前 `nvgpu_gr_obj_ctx_commit_global_ctx_buffers` 提交 pagepool / bundle cb / attrib cb / cb manager / rtv cb; 驱动没分配也没提交 → bundle 流排不空。
**已测排除**: 单独去掉 sw_ctx_load 的图形过滤 (561 条全写) 无效; `--skip 241` 纯 compute 配置 (eng_config=0x6 + 过滤) 同样卡在 934/935。
**修复** (`ga10b_gr.c`, 全部对照 nvgpu r36.4 非 MIG 路径):
- 分配 4 个全局 buffer (pagepool 0x20000 / bundle cb 0x3000 / attrib cb / rtv cb 0x80000), 映射进 golden VM, bundle 之前直写寄存器 (`gr_commit_global_ctx_buffers`);
- `gr_rop_mapping()`: map tiles + crstr/wwdx/rstr2d 映射表 + pd_dist_skip_table (fs_state_init 的非 MIG 部分);
- sw_ctx_load 不再过滤图形寄存器; init_hw_state 末尾补 `blcg_gr` prod; auto_go_idle 同时管 graphics(bit17)+compute(bit18); golden save 前写 zcull mode=no_ctxsw;
- 每通道 `ga10b_gr_ctx_prepare`: 全局 buffer 映射进通道 VM, 地址经 **patch buffer** 覆盖 golden 镜像里的 golden-VM 地址 (patch_count 写入 main header)。**已上机** (§2.6: 通道 ctx 被 FECS restore, 无 MMU fault, vadd PASS)。
- `--skip 200` 回到 MIG 式 (过滤 + 不提交全局 buffer), 仅作对照。

### 2.6 首次 compute launch: 四个问题逐个定位 (已测, 2026-11-24 下午)

`ga --hw compute` = `gr` + GRAPHICS runlist 上建通道 + 读 `/fs/cuda/vectorAdd.sm_87.cubin` (projects/cuda 的 nvcc 13.5.37 产物) + QMD launch + CPU 比对。cubin 解析与 QMD v3.0 编码直接编译 projects/cuda 的 `cubin.c`/`qmd.c` (host 单测过的那份), 没有另写一套。每个问题都是一次 fresh boot 一个现象:

| # | 现象 (寄存器证据) | 根因 | 修复 |
|---|---|---|---|
| 1 | sem 超时; `pbdma0 intr=0x01000000` (**eng_reset**), FECS cur_ctx=0, PB 一个字没取 | GR runlist 上还挂着 grcopy LCE0/1, RAMFC target 带 `ce_ctx_valid`; compute 路径没跑 `ga10b_ce_init`, LCE 还在 reset (mc_device_enable=0x1000) → PBDMA 报 eng_reset 并停住 | `cmd_compute` 在建通道前调 `ga10b_ce_init()` (nvgpu 的 ce_init_support 总在第一个通道之前) |
| 2 | PB 全取完, FECS cur_ctx=通道 inst (**ctx restore 成功, 无 MMU fault**); `gr_exception=0x1`, `fe_hww_esr=0x80000002 info=0xc7c0`, trapped subch0 mthd0 | GR FE 的类是按**固定 subchannel** 绑的 (3D=0, compute=1, i2m=2, 2D=3); 在 subch 0 上 SET_OBJECT 0xC7C0 被拒, info 就是被拒的 class | 新增 `GA10B_SUBCH_COMPUTE=1`, compute 方法全走 subch 1 |
| 3 | `gr_intr=0x00100000` (class_error), `class_error=4` (INVALID_VALUE), trapped 依次是 mthd 0x288 `SET_QMD_VERSION`(0x00300030) → 0x7b4 `SET_SHADER_LOCAL_MEMORY_WINDOW_B`(本地内存 GPU VA) → 0x298 `INVALIDATE_SKED_CACHES`(1) | 骨架里这几条方法的取值是猜的。WINDOW_A/B 是"本地内存在 shader 地址空间里的窗口基址", 不是 backing buffer 地址。**第一次 class error 后引擎进 faulted (engstat0 bit30), 后续方法全被丢弃**, 所以一次 boot 只能看到一个坏方法 | 发射序列收敛到 Mesa NVK 在 Ampere 上用的最小集: `SET_OBJECT` → `SET_SHADER_LOCAL_MEMORY_A/B` → `…NON_THROTTLED_A/B/C` → `SEND_PCAS_A(qmd>>8)` → `SEND_SIGNALING_PCAS2_B(3)`; cache 失效靠 QMD 里的 INVALIDATE 位 |
| 4 | (写代码时由反汇编发现, 未上机踩) | nvcc 生成的 SASS 从 **cbank0 头部**读运行时状态, 这块 CUDA 驱动在 launch 时填, cubin 里是 0: `c[0x0][0x0]` blockDim.xyz, `c[0x0][0xc]` gridDim.xyz, `c[0x0][0x28]` R1 初值 (线程栈指针), `c[0x0][0x118]` 64 位全局地址基 (.E 寻址, 填 0); 参数从 param_base=0x160 起。**projects/cuda libcuda 目前不填这块** | `ga10b_compute.c` 在上传 cbank0 时填 blockDim/gridDim/栈顶; libcuda 的 orin 后端 launch 必须照做 (§4) |

实测通过的 QMD: grid=4 block=256, regs=12, cbuf0=376B→0x180, release0 semaphore。结果 c[i]=a[i]+b[i] 1024/1024 一致 (a=i, b=2i+0.5)。
**推断/未验证**: c[0x0][0x28] 填的是 local 内存大小 (vadd frame=0, 没用到栈); 带栈帧的核、shared memory、多 cbank (bank2/3)、WMMA 都还没上机。SM 数据通路只验了 FADD/LDG/STG/IMAD。

诊断设施 (保留): launch 等待循环里轮询 `gr_intr`, 打印 trapped subch/mthd/data + class_error + fe_hww 后 W1C ack; 失败时 `ga10b_gr_launch_postmortem()` dump GR status/activity/exception、全部 hww ESR、每 TPC 的 SM warp/global ESR, 再接 `diag_dump()` (runlist/PBDMA/MMU fault)。

---

## 3. 已排除假设表 (每项都板上/L4T 复测 — 勿重复)

| # | 假设 | 验证方式 | 结论 |
|---|---|---|---|
| 1 | blob 布局/版本错 | v1 NON_FUSA header 2296B + DMEM NO-SAFETY 串互证 | 排除 (已测+源码) |
| 2 | NETC netlist 选错 | netlist_ga10b.h:45 = NETC_img_prod_encrypted.bin | 排除 (源码) |
| 3 | FECS/GPCCS 签名缺失 (0x98) | ACR-managed LS-PMU, PMU bootstrap 写签名 | **已解** (已测) |
| 4 | PMU VM aperture (coh vs ncoh) | L4T 页表 walk: PDB target=ncoh(3)/PDE=ncoh(6)/PTE=ncoh(6); 改 coh 上机无效 | 排除 (已测, **ncoh 才对**) |
| 5 | inst block subctx0 / ctx_bind | L4T: ncoh PDB + subctx0(w166=1/168/169) + **coh ctx_bind(0x60...)**; 已对齐 | 排除 (已测) |
| 6 | super surface VA 布局 | 试 L4T 高 VA 0x1FE10000 (原 0x10000000), 同样卡 | 排除 (已测, VA 非根因) |
| 7 | boot args 字段 | L4T DRAM 逐字: boot_type='NVRM'/size=184/version=1/core_dump_size=8192/cmdline_v7 全字段 (cpu_freq=0x0c28cb00/trace/ss params=0x01004000/0x01100000/flags=0/ctx_bind) 全一致 | 排除 (已测) |
| 8 | FBQ head/tail 寄存器/值格式 | head 0x10a800+4i/tail 0x10a820+4i/msgq 0x10a4c8/cc, 字段<<0, 与 hw_pwr_ga10b.h 一致 | 排除 (源码+已测) |
| 9 | hdr.address / super surface params | host 写 hdr.address=gpu_va (0x10600); params=(size&0xFFFFFF)\|(DMAIDX_VIRT1<<24) | 排除 (源码+已测) |
| 10 | ssmd 全 0 异常 | L4T ssmd 也全 0 → **正常**, RTOS 按需填 | 排除 (已测) |
| 11 | trace buffer 全 0 异常 | L4T trace 也全 0 → 正常 | 排除 (已测) |
| 12 | itfen ctxen / irqblk / falcon IRQDEST | itfen=1(ctxen), irqblk 0x500-51c, falcon IRQDEST=0, PRISCV IRQDEST=0x014000D0 全与 L4T 一致 | 排除 (已测) |
| 13 | cache 一致性 | DMA 池 NOCACHE; SYS_DMA_SYNC op0 clean+invalidate 无效; CE blit 同池同 aperture 已证 host→GPU 一致 | 排除 (已测) |
| 14 | MC PMU unmask / swgen kick | func top/leaf unmask 仅 PMU→host; host 写 RISC-V 本地 IRQ 块被 priv 过滤 (写回读 0) | 排除 (已测) |
| 15 | INIT ack 缺失 | host 写 msgq_tail=1 ack, RTOS 收到 (msg head/tail=1/1) | 排除 (已测) |
| 16 | PMU 卡点 = 只需清 falcon IRQSTAT | `--skip 100` 只写 IRQSCLR 不 ack dmesg → 仍 cmd_tail=0 | 排除 (已测); 真因见 §2.4 |
| 17 | golden 卡点 = sw_ctx_load 图形过滤 | 561 条全写, 仍卡 bundle 934/935 | 排除 (已测); 真因见 §2.5 |
| 18 | golden 卡点 = 该走纯 compute(MIG) | `--skip 241` 仍卡同一条 bundle | 排除 (已测) |

---

## 4. 下一步 (推断, 按顺序)

1. **ga10bd 常驻化 → `/dev/nvgpu`**: 现在是一次性 CLI, GPU 状态跨进程保留但驱动状态不保留 —— `gr`/`compute` 成功一次后同一次上电内再跑会在 `fecs ctxsw handshake timeout` 失败 (FECS 已在跑, 不会再报 INIT_DONE; 已测), 只能 `pico_reset`。常驻后 golden ctx 只做一次, 通道/VM/buffer 由 daemon 持有。接口面按 projects/cuda `struct cuda_backend` 反推: mem_alloc/free (DMA + GMMU map)、memcpy h2d/d2h (Tegra 统一内存, 直接映射)、module_load (code + cbank 上传)、launch (QMD + 提交 + semaphore 等待)。
2. **projects/cuda `backend_orin.c` 接线**: launch 路径补 cbank0 头 (§2.6#4); 然后板上 `deviceQuery → vectorAdd → 13 算子与 sim 对拍 → mini_model → mini_llm`。
3. **扩大 SASS 覆盖**再谈性能: 带栈帧的核 (验证 c[0x0][0x28] 语义)、shared memory (QMD SMEM_CONFIG + `SET_SHADER_SHARED_MEMORY_WINDOW`)、bank2/3 字面量、WMMA。每类先单核上机。
4. 清理项: 通道 TSG 拆除/preempt (审计 F7 遗留)、GR/PBDMA 中断仍是轮询、`--skip` 诊断位可在常驻化时收掉。

---

## 5. 代码与未提交足迹

**注意: `machines/` 是 git submodule**, 以下状态需要在 `machines/` 内查。

| 路径 (machines/ 内) | 状态 | 内容 |
|---|---|---|
| `nvidia/orin.nano/system/drivers/ga10bd/` | **整目录 untracked, 从未 commit** | 驱动全部: 11 个 .c + regs/fifo/mm/fw/netlist/ctx 头 + test/ (host 单测) |
| `nvidia/orin.nano/docs/ga10bd-audit/` | untracked | nvgpu 逐模块对照审计 4 篇 + README |
| `nvidia/orin.nano/efi/Makefile` | M (+16) | GPU_FW_DIR 打包 + ga10bd 进 SYSTEM_FILES 与 cp 配方两处 + `/bin/ga` alias |
| `nvidia/orin.nano/kernel/bsp/hw_info_arch.c` | M (+3) | GPU BAR0 进 `check_mem_map_arch` MMIO 白名单 |
| `nvidia/orin.nano/system/libs/bsp/{src/bsp_bpmp.c,include/bsp/bsp_bpmp.h}` | M (+13) | `bsp_bpmp_pg_get_state()` (CMD_PG_GET_STATE) |
| `nvidia/orin.nano/system/etc/init.ramfs.rd` | M (+6) | **仅注释**: ga10bd 永不自动启动, 手跑分级命令说明 |

**本会话 ga10b_pmu.c / ga10b_mm.* / ga10b_fifo.h 改动 (未 commit)**: PMU VM aperture 回 ncoh (PDB target3/PDE6/PTE6, L4T 页表 walk 证据); inst block 恢复 subctx0 (w166=1/168/169) + ctx_bind coh (0x60000000); ga10b_mm.h 编码器加 aperture 参数; ga10b_gmmu_t 加 aperture 字段 (CE/FIFO/GR 仍 ncoh); ga10b_fifo.h 加 ga10b_ramin_pdb_lo_ap(pdb,target)。pmu_vm_init 现有 `st.mm.va_next=0x1FE10000` (VA 实验, 已证非根因, 可回退 0x10000000)。

**2026-11-24 下午追加 (未 commit)**: `ga10b_pmu.c` dmesg 环读取 + ISR 仿真 (§2.4); `ga10b_gr.c` 全局 ctx buffer / rop mapping / 通道 patch (§2.5), 删掉了旧的 SCTL sigcheck-skip 诊断; `ga10b_acr.c` `ga10b_acr_bootstrap_ls` 改为校验 FECS HS 签名 (旧实现在 ACR-reuse 路径下因 `acr.ready==0` 误返回 -1, 表现为 `gr -> -1` 无任何报错); `ga10b.h`/`ga10bd.c` 新 skip 位 0x80 (无 PMU ISR 仿真) / 0x100 (ISR 仿真但不 ack dmesg) / 0x200 (MIG 式 golden); `test/test_integration.c` 断言同步 (sw_ctx_load 全写, auto_go_idle 0x60000)。

**compute 追加 (未 commit)**: 新文件 `ga10b_compute.c`; `Makefile` 在 `$(ewokos)/projects/cuda/libs/cuda/src/qmd.c` 存在时加 `-DGA10B_HAVE_CUDA` 并把 `cubin.c`/`qmd.c` 编进来 (没有 projects/cuda 时 `compute` 只走占位 QMD 的 dry-run); `efi/Makefile` 新增 `CUDA_CUBIN_DIR` (默认 `../../../../projects/cuda/cubin`) 打包 `*.cubin` 到 ramfs `/fs/cuda/`; `ga10b_gr.c` 拆出 `ga10b_compute_launch(ch, qmd[64])`, `ga10b_compute_dispatch` 只剩占位 QMD 包装; `ga10bd.c` `gr` 现在止于 golden ctx 并返回 0, 新命令 `compute` + `--cubin FILE`; `test/test_mm.c` 补上午加 aperture 参数后漏改的调用 (之前编不过)。6 个 host 单测全过。

**并行会话文件勿扫进 commit**: dceprobe/fand/fbdisplayd/orin_net.h (display 会话), qcom/8gen3/* (WiFi/电池会话), rockchip/ (RK3399 WIP), rollback-4k-20261123/ (display 回滚快照)。
**固件 blob 绝不入 git** (NVIDIA 专有, 只在 GPU_FW_DIR 存在时打包)。

### 5.1 驱动结构速览

- 独立 CLI, **不接 init.rd, 默认 dry-run** (写日志/读返 0), 硬件需显式 `--hw`。状态不跨进程持久: `ce`/`gr`/`pmu` 每次在进程内重跑前置链。
- 分级子命令: `fw` (blob 校验, 无 MMIO) / `probe` (只读) / `init` / `acr` / `pmu` (ACR-managed LS-PMU 安全启动+FBQ RPC) / `ce` (M0) / `gr` (到 golden ctx) / `compute` (M1, `--cubin FILE` 可换 cubin) / `diag` (只读) / `dump` / `power-off`。
- `stage_acr()` 有 **ACR-reuse 分支**: acr_done 时不 reset GR/不重跑 ACR (one-shot), 只重放 host/FB/FIFO init。需要 fresh ACR 时 reboot。
- compute: `ga10b_compute_launch(ch, qmd[64])` 提交任意已编码 QMD 并等 release0 semaphore; `ga10b_compute.c` 的 `ga10b_compute_vadd` 是目前唯一的真实调用者 (cubin → code/cbank 上传 → QMD → 比对)。`ga10b_compute_dispatch` 只是 dry-run/host 单测用的占位 QMD 包装。

---

## 6. 构建 / 部署 / 测试流程

### 6.1 构建链 (路径陷阱已踩平)

```bash
# 1. 驱动 (输出到顶层 system/build_aarch64/orin.nano/rootfs/drivers/orin.nano/ga10bd)
make -C machines/nvidia/orin.nano/system/drivers/ga10bd
# 2. 必须 final: 把顶层 rootfs 拷进 machine-local build_aarch64 (efi 只读后者!)
make -C machines/nvidia/orin.nano/system final
# 3. 打包 (SELF_ROOT=1 默认 = 16MB bring-up 串口救援镜像; GUI=1 / X=1 见 efi/Makefile)
make -C machines/nvidia/orin.nano/efi initramfs
# 4. 验证打包的 ga 确实是新构建 (曾踩过 stale 坑)
md5 machines/nvidia/orin.nano/efi/build/initramfs/bin/ga \
    machines/nvidia/orin.nano/system/build_aarch64/drivers/orin.nano/ga10bd
```

efi 目录即 TFTP/HTTP serving dir, 重打包即部署。内核白名单改动需重编 kernel (`make -C machines/nvidia/orin.nano/kernel`, objcopy 硬编码输出 kernel.bin)。
**快捷路径 (本会话用)**: `make -C ga10bd` → `cp -p system/build_aarch64/orin.nano/rootfs/drivers/orin.nano/ga10bd machines/nvidia/orin.nano/system/build_aarch64/drivers/orin.nano/ga10bd` → `make -C machines/nvidia/orin.nano/efi initramfs` (跳过 system final, 仅更新 ga10bd 时够用)。

### 6.2 host 单测 (macOS 本机, 无需板)

```bash
cd machines/nvidia/orin.nano/system/drivers/ga10bd
cc -I. -Itest -Itest/host -Wall -Wextra -o /tmp/test_integration \
  test/test_integration.c test/host_shim.c ga10b_gr.c ga10b_fifo.c \
  ga10b_mm.c ga10b_ce.c ga10b_fw_parse.c
/tmp/test_integration   # 默认 fw_dir=/Users/tony/work/gpu-fw/ga10b
```

test_integration 断言真实驱动流的 MMIO 序列 + NETC blob 内容 + RAMFC/RAMIN/GPFIFO/USERD 结构。改 GR 序列必须同步改断言。另有 test_fw/test_mm/test_fifo/test_ctx/test_netlist。

### 6.3 板上迭代 (rig 详见 memory reference-orin-test-rig)

```bash
python3 machines/nvidia/orin.nano/test/autoboot.py login   # 快速 boot ≈十几秒, 失败重试一次
python3 /tmp/fancmd.py "/drivers/orin.nano/ga10bd --hw pmu" 30   # 设 Pico tx_delay_us(默认20ms)+bulk send 防吞字; ga 不在 PATH 用全路径
python3 /tmp/pxe_call.py serial_log '{"action":"read","lines":120}'
```

- board-pxe MCP TCP 127.0.0.1:8899 (root 运行, 勿起第二实例); Pico 桥 cafe:4004, `pico_reset` 硬复位。
- **klog 不是 slog** (bring-up 镜像无 /dev/log, slog 全丢)。
- 发命令前确认串口在 Orin 上 (TW220 共用 rig, 适配器会移)。
- 板上验收 (每条前 `pico_reset`, 见下): `ga --hw ce` → "CE blit PASS"; `ga --hw pmu` → `PMU RPC func=0 OK` / `func=3 OK`; `ga --hw gr` → `fecs ctxsw INIT_DONE` / `golden ctx saved` / `gr -> 0`; **`ga --hw compute` → `vadd PASS (1024 elements, 4 blocks x 256 threads)` / `compute -> 0`**。compute 之后同一次上电内 `ce` 仍 PASS (已测)。
- **每次 fresh 验证先 `pico_reset`**: ACR/PMU 状态跨 EwokOS 进程保留 (BPMP 拒绝 CCPLEX 关 GPU 电), 一次失败的 pmu 会把 RTOS 留在卡死态, 后续 `pmu`/`gr` 都不可信。一键脚本: `/tmp/ga_run.sh "<ga 参数>" <等待秒> <输出文件>` (reset → 等 login → 登录 → 设 tx_delay_us+bulk 发命令 → 存串口日志; /tmp 易失, 丢了照 §6.3 三条命令重写)。
- **串口防吞字 = MCP 固件字符间延时, 非逐字符慢发**: 多词命令 bulk `serial_log send` 在默认 1000µs TX 延时下可能丢字 (板端 HS-UART RX FIFO 32B 溢出)。解法 = `pico_gpio {"action":"set","tx_delay_us":<µs>}` 抬高 Pico 固件 TX 字符间延时 (固件控制, 主机不做软件 sleep; 持久化 pico/gpio-layout.conf, 设备在位自动应用; 默认 1000, 0=全速), 再 bulk send。`/tmp/fancmd.py "<cmd>" [wait] [tx_delay_us]` 已封装 (默认 20000µs=20ms); 仍吞字就调高, 板端 RX 跟得上就调低提速。

### 6.4 EwokOS ↔ L4T 切换 (全程序化, 2026-11-24 用户配置)

默认启动项 = PXE, SD 常驻在位不用插拔:
- **启 EwokOS**: `pxe_control {"action":"start"}` → `pico_reset`。
- **启 L4T (SD)**: `pxe_control {"action":"stop"}` → `pico_reset` (UEFI 网络尝试失败 ~2min 后自动 SD)。
- L4T GPU boot: initramfs bash → `mount /dev/mmcblk0p1 /mnt` + chroot proc/sys/dev mounts → 固件预置 `cp /mnt/lib/firmware/nvidia/ga10b/*.* /lib/firmware/nvidia/ga10b/` → `chroot /mnt /sbin/modprobe nvgpu` → `echo on > /sys/devices/platform/17000000.gpu/power/control`。

### 6.5 L4T DRAM 读取 (rdram.ko, 已测)

L4T `CONFIG_STRICT_DEVMEM=y` 挡 devmem/dd 读 System RAM, `/proc/kcore` 缺失, extlinux APPEND 被 bootloader 动态 cbootargs 覆盖 (iomem=relaxed 无效)。**解法 = 自编内核模块 phys_to_virt() 读线性映射**:
- 源码 `/mnt/tmp/rdram/rdram.ko` (SD rootfs 持久), Makefile `obj-m += rdram.o` + `$(MAKE) -C /lib/modules/5.15.148-tegra/build M=/tmp/rdram modules`。
- 构建: `chroot /mnt /bin/bash -c 'export PATH=/usr/sbin:/usr/bin:/sbin:/bin; cd /tmp/rdram && make'` (内核源树在 /mnt/usr/src/linux-headers-5.15.148-tegra-ubuntu22.04_aarch64/3rdparty/canonical/linux-jammy/kernel-source)。
- 加载: `busybox rmmod rdram` (chroot rmmod 无 /proc/modules 会失败) → `chroot /mnt /sbin/insmod /tmp/rdram/rdram.ko addr=<phy>` (读 64 字) 或 `pdb=<PDB_phy> va=<gpu_va>` (走 5 级页表读目标页)。
- 地址每次 boot 变: mailbox 0x1710a040/44 → boot_args phy = ((mb1&0xff)<<32)|mb0; new_instblk 0x1710a480 → inst phy = (val&0x0fffffff)<<12; inst w128/129 → PDB phy; boot_args cmdline_v7 @偏移 0x78 (cpu_freq@w30/trace va@w31/ss va@w41/ctx_bind@w45)。
- dmesg 刷屏会弄乱串口捕获, 用 `dmesg | grep 'rdram.<addr> .<off>.' | tail -1` 定向重读。

---

## 7. 里程碑

1. ~~M0 CE blit~~ / ~~PMU FBQ~~ / ~~FECS INIT_DONE~~ / ~~golden ctx~~ / ~~M1 首次真实 compute (vadd)~~ — **已完成 (已测, 2026-11-24)**, 见 §2.4–§2.6。
2. **M2**: ga10bd 常驻 `/dev/nvgpu` + projects/cuda orin 后端 (§4 第 1、2 条)。
3. **M3+**: 板上 compute 套件 (deviceQuery → vectorAdd → 13 算子对拍 → mini_model/mini_llm), 之后才是量化 GEMV / WMMA 性能化。

---

## 8. 关键寄存器速查 (BAR0 偏移)

| 寄存器 | 偏移 | 备注 |
|---|---|---|
| FECS falcon base | 0x409000 | CPUCTL_ALIAS 0x409130 (=0x2 start); HS sig io 0x280-28c (GR 0x409280-28c) |
| GPCCS falcon base | 0x41a000 | alias 经 FECS+0x11000 = 0x41a130 |
| GPC0 GPCCS base | 0x502000 | mbox6 @0x502818 |
| FECS ctxsw mbox0/1/6 | 0x409800/04/18 | mbox0=INIT_DONE 握手, mbox6=error_code |
| gr_intr / en / notify_ctrl | 0x400100 / 0x40013c / 0x400160 | W1C; mask 0x7901d3; bit31=CPU bit30=GSP |
| gr_exception / class_error / trapped addr,data | 0x400108 / 0x400110 / 0x400704, 0x400708(lo) 0x40070c(hi) | exception bit0=FE; gr_intr bit20=class_error; class_error 4=INVALID_VALUE; trapped addr: mthd 13:2, subch 18:16 |
| fe_hww_esr / info | 0x404000 / 0x4041b0 | 0x80000002 + info=class → 该 subch 不接受这个 class |
| SM0 warp_esr / global_esr | 0x504730 / 0x504734 (+0x800/TPC) | SASS 执行异常看这里 |
| pbdma_intr_0 | bit24 = eng_reset | 目标引擎 (含 runlist 上的 grcopy LCE) 在 reset |
| runlist engine_status0 | rl+… bit30 = faulted | GR 报错后置位, 之后方法全丢 |
| mc_enable | 0x200 | bit30=GR |
| priv_sec_en fuse | 0x820434 | =1 (零售已熔) |
| **PMU falcon base** | 0x10a000 | IRQSTAT 0x008, IRQMASK 0x018, IRQDEST 0x01c, ITFEN 0x048(ctxen bit0), OS 0x080, HWCFG2 0x0f4(priv_lockdown bit13), CPUCTL 0x100 |
| **PMU RISC-V (PRISCV) base** | 0x10b000 | CPUCTL 0x388, IRQSSET 0x520, IRQMASK 0x528(**bit22=FBQ**), IRQDEST 0x52c, BCR_CTRL 0x668, irqblk 0x500-51c |
| **PMU new_instblk** | 0x10a480 | RTOS 自绑 VM = inst_ptr>>12 \| valid bit30 \| sys_coh 0x60000000 |
| **PMU FBQ cmd head/tail** | 0x10a800+4i / 0x10a820+4i | i=phy queue idx (LPQ=1); 字段<<0 |
| **PMU FBQ msg head/tail** | 0x10a4c8 / 0x10a4cc | head=PMU 写 host 读, tail=host 写 PMU 读 |
| PMU boot args mailbox | 0x10a040 / 0x10a044 | = args_phy + EXTMEM2(0x8060000000000000) |
| falcon debug (base 相对) | CURCTX 0x050, DEBUG1 0x090, DEBUGINFO 0x094, IMSTAT 0x144, TRACEIDX 0x148, TRACEPC 0x14c, EXTERRSTAT 0x16c | exterr 非 priv 读 = 0xbadf5040 poison |

---

## 9. 3D 加速 API (portablegl / GLES 走 GPU) — 2026-11-24

### 9.1 结论与路线 (源码)

EwokOS 的 3D 加速 API = `portablegl → g2dclient(g2d_pgl_program/draw) → g2dd → bsp_g3d`。现役路径只有"编译程序"一条 (固定管线 fs_id 路径在客户端和两个真实后端都是关死的), 而网格程序**只编译 `geom_raster_cs.h` 里 9 个固定 GLSL 计算核**, 应用自己的 VS/FS 从不编译。所以 Orin 不需要板上着色器编译器:

- 把固定核移植成 CUDA C, 用 nvcc 离线编成 sm_87 cubin, 嵌进驱动库;
- Orin 的 pglcompiler 后端按**核文本精确匹配**返回预编译核的 id + uniform 布局表 (8gen3 Adreno 后端替换 raster 核用的同一手法);
- 全屏 fragment 特效 (ocean/grass/shadertoy 类, 需要真 GLSL 编译) 一律 decline, 留在 CPU。
- 9 个核 (5 个顶点变换变体 + 光照 gouraud + 3 个光栅变体) 已全部有对应的 sm_87 核。

### 9.2 组成 (全部未 commit)

| 位置 | 内容 |
|---|---|
| `system/libs/ga10b/` | 驱动库 libga10b.a。新增 `ga10b_dev.c` (bring-up 阶段 + 进程级设备: 一个 VM + 一个 compute 通道, `ga10b_dev_open/map/alloc/module/launch`), `ga10b_compute.c` (cubin 模块加载 / 核 launch), `ga10b_pgl.c` (PGL 守护进程侧), `ga10b_pgltest.c` (CLI 自测) |
| `system/libs/ga10b/kernels/` | `pgl_geom.cu` (核源码, 单源: nvcc 或 `-DPGL_SIM` 当 C 编作 CPU 参考), `pgl_sim.h`, `build.sh` (VM 里跑 nvcc, 同时生成嵌入头 `pgl_geom_cubin.h`), 产物 `pgl_geom.sm_87.cubin` |
| `system/libs/ga10b/include/ga10b/` | 公共头 `ga10b_dev.h`, `ga10b_pgl.h` (含 blob 格式 `ga10b_prog_blob_t`、uniform 偏移、版本码 **1087**) |
| `system/libs/nvcompiler/` | pglcompiler 后端 `nv_pglc.c` (无编译器; 文本匹配 → 核 id) |
| `system/libs/bsp/src/bsp_g3d.c` | 转发到 libga10b; **门控 `GA10B_ENABLE=1`** (环境变量或 `/tmp/ga10b.cfg`), 未开时全部 -2; `GA10B_PROF=1` 每 16 次 draw 打一行分阶段耗时 |
| `system/libs/bsp/src/bsp_g2d.c` | `bsp_g2d_version()` 在 GPU 起来后返回 1087 |
| `system/make.inc` | `PGL_G3D=yes` + `PGL_G3D_FRONTEND=no` + `PGL_G3D_COMPILER_DIR`; `BSP_LFLAGS=--start-group -lbsp -lga10b --end-group` |
| `system/drivers/g3dtest/` | 控制台端到端自测 (不需要 X) |
| `projects/opengl/libs/gles/Makefile` | 新开关 `PGL_G3D_FRONTEND=no`: 不构建/不合并 mesa GLSL 前端 (**projects 子模块里的改动**) |
| `efi/Makefile` | bring-up 镜像带 g2dd + g3dtest + cubin; X 镜像带 GPU 固件 |

### 9.3 光栅器: 为什么不是照搬 GLSL 核 (已测)

GLSL 原核是"一个线程管一个三角形, 串行扫包围盒"。照搬到 GA10B 后, 12 个大三角形 (640x480 上的立方体) 一帧 **90 ms**: 单个 GPU 线程很慢, 大三角形把一个线程拖几十毫秒。现在守护进程跑的是 **tile 光栅器** (`pgl_tri_setup` → `pgl_bin_sort` → `pgl_raster_tile`): 每三角形一个线程做 setup 并按包围盒挂到 16x16 tile 的链表; 每 tile 一个线程按三角形序号排序; 每**像素**一个线程顺序遍历本 tile 的链表。像素只有一个 owner, 深度测试/写入/混合都是顺序代码, 结果等价于按提交顺序逐个画, 没有原核的跨三角形竞争。帧清除并进像素核 (首帧标志), 不占额外 launch。

| 640x480 | 原核 (每三角形一线程) | tile 光栅器 |
|---|---|---|
| 全场景 2012 三角形 | 84 ms | **1.4 ms** |
| 仅 12 个大三角形 | 90 ms | 1.1 ms |
| 仅 2000 个小三角形 | 13.8 ms | 1.2 ms |

另一个量级问题是等待方式: launch 完成原先靠 `usleep` 轮询, 一次至少一个调度 tick (~1 ms); 现在先按时间自旋 4 ms 再退回睡眠轮询 (上表 tile 列里 4.1 ms → 1.4 ms 就是这一项)。

三种光栅变体 (顶点色 / 最近邻纹理 / 双线性纹理×顶点色+混合+裁剪) 共用这一组 tile 核, 由守护进程按 raster 核 id 从 uniform 区推导出一个参数块 (`ga10b_pgl_fill_bp`, 守护进程和 g3dtest 共用) 来选模式。

对客户端的契约 (blob、uniform 名、vout/fb/depth 缓冲) 没变。uniform 块 = [xform 区 | raster 区], 按 blob 里的 `xform_unif_dwords` 切。

### 9.4 验证方法

```
# bring-up 镜像 (串口, 无需看屏)
echo GA10B_ENABLE=1 > /tmp/ga10b.cfg
export GA10B_PROF=1                      # 可选: 每 16 次 draw 一行分阶段耗时
/bin/ipcserv /drivers/orin.nano/g2dd /dev/g2d    # 期望 "orin g3d: GA10B bring-up OK"
/drivers/orin.nano/g3dtest               # 期望 7 行 PASS + "g3dtest: PASS (0 of 7 scenarios failed)"
/drivers/orin.nano/g3dtest map           # 映射淘汰压力: 期望 "map stress PASS"
# X 镜像 (make -C efi initramfs X=1): 同样三行之后
/bin/bgrun /apps/cube/cube               # 也可 teapot / 3ddemo / matrix
```

`g3dtest` 的 7 个场景 = colour / colour+blend+cull / lit / textured / instanced / instanced-lit / texcol+scissor。每个场景走真实客户端路径 (后端按文本"编译" → 序列化 → `g2d_pgl_program` → `g2d_pgl_draw`), 再用同一份核源码在 CPU 上顺序跑一遍, 逐像素比颜色 (±2) 和深度。实测每场景超容差像素 0–5 个 (三角形边缘, GPU 的乘加融合所致)。

真实应用里已见到的 kind (串口 profile, 已测): cube/teapot = kind 1 (网格) + kind 4 (ImGui 叠加层); 3ddemo = kind 2 + kind 4; matrix = kind 4 (1920x1080, ~5500 三角形, ≈1.3 ms/draw)。**kind 3 (实例化) 只有 g3dtest 覆盖, 没找到用它的现成应用。**

- `ga --hw raster` (CLI, 不经 g2dd): 旧的每三角形核 vs CPU 参考, 512x384 / 1012 三角形, 仅 1 像素超容差 (已测)。
- 构建: 系统用 `make -C machines/nvidia/orin.nano/system x` (干净 SDK 树要先 `HW=orin.nano ARCH=aarch64 ARCH_VER=v8 make -C system/network/libs/socket`, 否则 netd 找不到 `sys/socket.h`); GL 栈用 `/tmp/orin_gl.mk` (imgui → opengl → cube), 产物要手工 `cp -Rp` 到 `system/build_aarch64/apps/`。
- **X 镜像不能超过 64MB** (loader 上限, 超了报 `lz4: bad sizes` / `http bad length` 起不来)。所以镜像里只留了 cube / teapot / 3ddemo / matrix / vkteapot, 其余 GL demo (ocean 等纯 CPU 的、Fireworks、imgui_demo、glmatrix) 编过但没打包。
- `/tmp/ga_build.sh` (增量构建+打 bring-up 包+host 单测) 与 `/tmp/ga_seq.sh <out> "<cmd>" <秒> ...` (复位→登录→顺序发命令) 是本轮用的脚本, /tmp 易失。

### 9.5 本轮修掉的两个稳定性问题 (已测)

- **映射只增不减**: 客户端缓冲现在映射在一个独立的 VA 窗口里 (64 个槽 x 64MB, 基址 0x1_0000_0000), 满了按 LRU 原地重映射 (`ga10b_gmmu_map_at` / `ga10b_gmmu_unmap`), 窗口和页表不再增长。页表池从 32 张提到 1024 张 (32 张只够映射约 58MB, 一个 1080p 的 fb+z 就占 17MB)。`g3dtest map` 用 96 个两种尺寸的画布来回清屏回读验证。
- **并发**: g2dd 的请求在多个 IPC 工作线程上并发执行, 而驱动只有一个通道/一条 pushbuffer/一组暂存缓冲。三个应用同时画时出现过 `sem timeout want=124640 got=124637` → 3D 被关掉。现在 `ga10b_pgl.c` 的每个入口都在一把互斥锁里。修后 4 个 g3dtest 并发全 PASS, 4 个应用并发 2.5 分钟无错。

### 9.6 已知限制 / 下一步

1. **g2dd 目前是串口手工起的**, X 的 init.rd 没接。原因是既有结论: `/dev/g2d` 一出现, displayd/xserverd 会改用 contig (内核映射为 uncached) 画布, 合成器变慢。本轮做法是**桌面起来之后**再起 g2dd, 桌面进程已经把 has_g2d() 判否锁存, 只有后启动的 GL 应用用到它。是否就这样接进 init.rd 待定。
2. **等待 GPU 靠自旋**: launch 完成前先忙等最多 4 ms, 所以 4 个应用同时画时 g2dd 占掉一个核的 ~85%。要降下来得接 GPU 中断 (现在全程轮询)。
3. **一次 launch 超时就把 3D 路径整个关掉** (返回 -3, 客户端退回 CPU), 没有通道/引擎恢复。
4. tile 链表容量 255 (1080p 128, 4K 32), 溢出的 tile 退化为全量扫描 (正确但慢); teapot 的 sort/raster 耗时偏高疑与此有关 (推断, 未量化)。binning 只按包围盒; 每帧整屏的 tile 都会 launch, 没用客户端给的 dispatch 窗口。
5. 需要真编译 GLSL 的程序 (全屏 fragment 特效、Vulkan 的 kind 5 用户着色器) 这条路线做不了, 归 NVIDIA 板上编译器那条线。vkteapot / vktest 在 Orin 上跑的是 CPU 软件设备 (vktest PASS, 90/90, 已测)。
6. `bsp_cl` (OpenCL / 之后 CUDA 算子的入口) 还是 stub; compute 通道现在归 g2dd 进程所有, 接 libcuda 时走这里而不是另起驱动。

### 9.7 顺序无关路径 + 帧时间去向 (2026-11-24 晚, 已测)

**改动**: 深度测试 + 深度写入 + 无混合的 draw (`BP_FLAG_UNORDERED`, 由 `ga10b_pgl_fill_bp` 判定) 不再 launch `pgl_bin_sort`; `pgl_raster_tile` 在深度相等时让小序号三角形胜出, 结果与按序合成逐位一致。这类 draw 的 tile 链表容量不再受 255 限制 (取 `PGL_LIST_WORDS / tiles`, VGA 下 873)。混合 / 不写深度 / TEXCOL 的 draw 仍走排序 + 255 上限。`GA10B_PROF=1` 的输出多了 tile 占用 (`bins used/avg/max/cap/ovf`)。

主机测试 `test/test_pgl_order.c`: 打乱每个 tile 的链表后与排序版 0 像素差异 (含部分/全部溢出的容量), 对照组 (旧规则 + 乱序) 约 3.5 万像素差异。板上 g3dtest 7/7 PASS, 各场景 colour_bad / depth_bad 计数与改动前相同。

| teapot 640x480, 6400 三角形, 单开 | 改前 | 改后 |
|---|---|---|
| sort | ~1.8 ms | 1–2 µs (不 launch) |
| raster | ~2.2 ms | ~0.42 ms |
| g2dd 内全部阶段 | ~4.2 ms | ~0.58 ms |
| 帧率 | 63 fps | 82 fps |

raster 的 2.2 ms 确实来自溢出: 最满的 tile 有 300–683 个三角形 (avg 55–64), 旧上限 255 下这些 tile 每像素扫全部 6400 个。

**GPU 已不是瓶颈, 帧时间去向 (`ps` 实测)**:

- teapot 单开 82 fps: teapot 进程占 65% 核 → 约 7.9 ms/帧是客户端自己的 CPU (其中 swap 1.34 ms); g2dd 15% → 约 1.8 ms/帧; 应用循环 `usleep(1000)` 约 2 ms。GPU 阶段只有约 0.8 ms/帧。
- 三应用同开: 核 0–2 只剩 31–36% 空闲, 核 3–5 是 96–99% 空闲。内核 `core_fetch` 只在创建时按任务数选核, 没有迁移; IPC worker 固定跑在客户端所在核。实测 cube 落在核 0 (与 xserverd、fbdisplayd 同核), 3ddemo 与 xwm_misa (34%) 同在核 2。teapot 稳定锁在 50 fps (单开 82), 原因未定位。
- g3dtest 的客户端往返 (`frame=`) 比 g2dd 内阶段之和多出 0–0.45 ms/draw, 不同启动之间差别大, 原因未定位。

**下一步候选 (都不在 GPU 核里)**: 给 GL 库加 `PGL_PROFILE` 门控的客户端分段计时, 拆开 teapot 那 7.9 ms (应用侧 CPU 光照、引擎逐顶点 staging、写 uncached shm、swap 拷贝); 内核按负载选核 / 迁移; demo 循环的 `usleep(1000)`。

### 9.8 客户端分段计时 + uncached 读回修复 (2026-11-24 晚, 已测)

**计时** (`projects/opengl`, 仅 `PGL_PROFILE=1` 时生效): 每 5 秒多一行
`PGL_PROFILE cpu draw_calls= tess= bufdata= clear= gpu_draws= stage= ipc=` (窗口累计, 除以 `frames` 得每帧)。`tess` = glDraw* 内引擎自身的 CPU; `stage` = `pgl_g3d` 把顶点流写进 shm 画布; `ipc` = g2dd 往返 (含 GPU 执行); `stage`/`ipc` 已从触发提交的那个入口里扣掉。

**根因**: `draw_mesh_compiled_slot` 为 `uDispatchOffset` 像素窗口算包围盒时, 逐顶点从 NOCACHE 的 `_pos` shm 读回坐标。Orin 的核没有这个 uniform, 算完即丢; teapot 每帧 19200 顶点, 这一段占 5.8 ms。
**修复**: 程序没有 `uDispatchOffset` 时不跑这段循环 (有该 uniform 的后端行为不变, 它们仍在读 uncached 内存 —— 改成读缓存里的源数据是等价的, 但没有可验证的板子, 未动)。

| teapot 单开, 每帧 ms | 修前 | 修后 |
|---|---|---|
| stage | 5.78 | 0.36 |
| tess | 1.03 | 0.87 |
| ipc | 1.00 | 0.90 |
| swap (其中 blit) | 1.66 (1.40) | 1.37 (1.21) |
| clear + bufdata | 0.23 | 0.20 |
| 其余 (应用 CPU + 循环睡眠) | 3.0 | 2.9 |
| 帧时间 / 帧率 | 12.7 / 78.6 fps | 6.6 / 150.7 fps |

三应用同开 (这次启动各进程落在不同核, 各核空闲 57–90%): cube 150.8 fps, teapot 82.5 fps, 3ddemo 117.6 fps (9.7 时为 68–74 / 50 / 87–90)。teapot 在三应用下每帧仍有约 8 ms 不在任何已计时段里 (单开只有 2.9 ms), 进程 CPU 只有 27%, 即在等待; 原因未定位。

**构建注意**: 这一轮的 GL 栈是在 `/tmp/ewk-prof` 的 CoW 克隆里编的 (索引里的 text-match `nv_pglc.c` + `PGL_G3D_FRONTEND=no`), 因为共享树里 `libs/nvcompiler` 有另一个会话在改。只把 cube / teapot / 3ddemo 三个二进制拷回 machine-local build 目录再打包。

### 9.9 demo 循环睡眠 + 帧间空闲 (2026-11-24 晚, 已测)

**计时再细分**: `PGL_PROFILE cpu` 行多了 `idle=` 和 `app=`。`idle` = 上一帧 `eglSwapBuffers` 返回到本帧第一次 `glClear` (应用事件循环: `x_run` 每帧一次 `X_DCNTL_GET_EVT` IPC + 循环里的睡眠/让出); `app` = 第一次 `glClear` 到 `eglSwapBuffers` 之间扣掉 GL 调用后的应用自身 CPU。

**根因**: cube / teapot / 3ddemo 的循环每帧 `usleep(1000)`。libc 对 >200 µs 走 `SYS_USLEEP` park, 内核按 tick 递减, 实际约 2 个 tick; 实测 idle = 1.85–2.07 ms/帧, cube 近一半帧时间在睡。
**修复**: 三个循环改成 `sched_yield()` (保留调度点, 不 park)。

| 单开 | 改前 | 改后 |
|---|---|---|
| teapot | 146 fps, idle 1.85 ms | 204 fps, idle 0.27 ms |
| cube | 234 fps, idle 2.07 ms | 401 fps, idle 0.13 ms |

改后每帧 ms — teapot 4.90: swap 1.30 / app 1.06 / ipc 0.88 / tess 0.81 / stage 0.36 / idle 0.27 / clear+bufdata 0.21。cube 2.49: swap 1.48 / ipc 0.60 / clear 0.17 / idle 0.13 / app 0.05 / tess 0.04。代价: 应用占满所在核 (cube 单开时核 0 空闲 1%)。
teapot 的 `app` 1.06 ms 基本是 `compute_colors()` (19200 顶点的 CPU Blinn-Phong); 引擎有 GPU 光照路线 (VS 声明 `uNormalMat` → lit 核), 但换过去会改变 teapot 的着色模型, 未动。

**三应用同开, idle 仍是大头且随桌面活动剧烈波动** (GL 各段稳定不变):

| 5 秒窗口 | cube | 3ddemo | teapot | idle (ms/帧) |
|---|---|---|---|---|
| 刚拉起 | 203–216 fps | 165–167 | 114–129 | 2.0–3.3 |
| 桌面有操作 (期间启动了 xprocs) | 54–69 | 43–78 | 37–60 | 9–21 |
| 之后 | 103 | 116 | 95 | 4.8–7.1 |

单开时 idle 只有 0.13–0.27 ms, 所以多出来的是 `X_DCNTL_GET_EVT` 在等 xserverd。Orin 的 `etc/x/x.json` 是 `multi_task: 0`: IPC 和合成 (`loop_step`) 在同一个任务上串行。9.8 里"三应用下 teapot 每帧约 8 ms 不在已计时段里"就是这一项。`ps` 同时看到 xwm_misa 53%、xserverd 21%。未定位到 xserverd 内部哪一步在占时间。

### 9.10 swap 拷贝: NC 读基准 + 多线程呈现 (2026-11-24 晚, 已测)

**微基准** (`ncbench`, 只在 /tmp/ewk-prof 克隆里, 二进制进了镜像 /bin/ncbench; 只读写自己申请的缓冲区):

| 方向 | 1 线程 | 2 | 3 | 4 |
|---|---|---|---|---|
| NC→缓存 (swap 的读方向) | 1.07 GB/s | 2.15 | 3.16 | 4.19 |
| 缓存→缓存 | 12.3 GB/s | — | — | — |
| 缓存→NC (写) | 10.0 GB/s | — | — | — |

640x480 / 1080p / 4K 三档一致: 单核读 NC 恒 1.07 GB/s (memcpy 只有 0.53, NEON 与 8 字节整数读相同 → 限制在总线请求数); 多核近似线性。写方向不慢。4K 一帧单线程 30.9 ms, 4 线程 7.9 ms。

**修复**: `egl_api.c` 的呈现拷贝对"源是 contig 缓冲的整帧 1:1 拷贝"按行分条带交给常驻线程池 (`present_blt`), 每条带仍走 `graph_blt`, 像素不变。`PGL_PRESENT_THREADS` 设总拷贝线程数 (默认 4, 1=单线程对照); `EGL_PROFILE` 多 `par=` 计数。缩放/信箱/小拷贝仍单线程。

| 单开 | 9.9 态 | 多线程呈现 |
|---|---|---|
| teapot | 204 fps, blit 1.15 ms | 236 fps, blit 0.36 ms |
| cube | 401 fps, blit 1.33 ms | 769 fps, blit 0.33 ms |

同镜像 `PGL_PRESENT_THREADS=1` 对照: cube 470 fps, blit 1.16 ms, par=0。三应用同开: cube 219 / 3ddemo 175 / teapot 98 fps (9.9 态 103 / 116 / 95), blit 0.6–0.7 ms; idle 仍 2–4 ms (9.9 的 xserverd 等待, 未动)。

**已知限制**: 工作线程在首帧创建, `core_fetch` 把它们都放到了应用所在核 (ps 显示 cube [3t] 单核 115%), 加速来自同核多线程的更多在途读 (3.6×), 不是跨核。基准里启动时创建的线程能落到空闲核 (4.19 GB/s)。要跨核需要内核提供选核手段或创建时错开负载统计 —— 未做。多应用时这些线程与 xserverd 抢同一个核的风险由此而来。

