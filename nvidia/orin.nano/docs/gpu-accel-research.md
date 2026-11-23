# Orin Nano / Tegra234 GPU 加速实现方案调研

日期：2026-11-22
范围：2D 加速 + 3D 加速可行性、路径、工作量、风险与落地接口。
标注约定：**已测** = 本机硬件/二进制实测；**documented** = 开源代码/官方文档核实；**推断** = 由事实推导、尚未上机验证。

> 现状前提：fb/display + GUI console + xwin 桌面已 COMMITTED 且目视确认，纯 CPU/NEON 合成即可点亮桌面。**GPU 加速是下一步性能优化，不是点亮的必需条件。** 显示扫描输出本身不需要 GPU（见 `project-orin-display-research`：直接线性 FB handoff）。

---

## 0. 结论速览

| 方向 | 首选路径 | 性质 | 关键缺口 |
|---|---|---|---|
| **2D 加速** | **VIC**（host1x 专用 2D 引擎，class 0x5D） | 明文可分发固件、**无安全启动链**、CCPLEX 可直接访问 | class 0x5D 的 2D 方法表不在任何开源代码 → **需 Orin TRM VIC 章** |
| **3D 加速** | **GA10B port L4T nvgpu** | 签名+部分加密固件，但 L4T 用同一份开源代码+官方 blob 点亮零售片 | 移植工作量 + 必须用 NVIDIA 专有固件二进制（**非密码学不可能**） |
| **compute-2D** | GR 通后走 compute channel，复用 **Mesa NAK**（Rust MIT，NIR→SASS，覆盖 Ampere sm87） | 比 full GL/Vulkan 现实得多 | 需自写薄 UMD 把 SASS+QMD 塞进 nvgpu channel |

**推荐：2D 短期走 VIC，3D 长期可选 nvgpu。** full GL/Vulkan 自写 UMD = 人年级，不建议。

---

## 1. 硬件 IP 事实（documented）

- **GPU IP = GA10B（Ampere），不是 PCI 设备**：`gpu@17000000`，compatible `nvidia,ga10b`，芯片识别经 PMC `boot_0`。
  - BAR0 寄存器 **0x17000000 + 16MB**，BAR1 **0x18000000 + 16MB**，fuse 窗口 0x03b41000。
  - IRQ：GIC SPI 67/68/70/71。
  - BPMP：**PG 35（GPU）、reset 19、clocks GPUSYS=304 / GPC0CLK=41 / GPC1CLK=236**（NAFLL_GPUSYS=428）——全部走现有 `bsp_bpmp` MRQ 通道（已测可用）。
- **SMMU 不是 GPU 的障碍**：ga10b DT 节点无 `iommus`，tegra234-mc.h 无 GPU stream ID；dma-coherent 经内部 GMMU 直接走物理 RAM。
- **priv_sec_en fuse @ BAR0+0x820434**：零售 T234 几乎肯定已烧（推断，时钟开启后运行时可读）。若置位，所有 falcon 必须经 ACR/WPR2 走 LS（low-secure）启动。
- **VIC 2D 引擎**：`vic@15340000 + 0x40000`，host1x **class 0x5D**，IRQ GIC **SPI206**（host1x syncpt SPI448-455 / host1x SPI263），BPMP **CLK_VIC=167**（NAFLL_VIC=207）、**RESET_VIC=113**、**POWER_DOMAIN_VIC=29**，SMMU **SID 0x34**（smmu_niso1；host1x 取指 SID0x27，VM context SID0x35-0x3C）。挂 host1x@13e00000（common/hypervisor/**vm 0x13e40000 = CCPLEX 段**），与 nvdec/nvenc/nvjpg/ofa/nvdla/pva 同级。

---

## 2. 2D 加速：VIC 路径（首选）

### 2.1 前提纠正
先前 grep "公开 DTS 无 vic 节点" 是 **v6.0 前快照偏差**。mainline **v6.0+（2022-10）有完整 vic 节点**（tegra234.dtsi `vic@15340000`，compatible `nvidia,tegra234-vic`，已测 grep 核实）。Orin Nano **未熔断/未禁用**（L4T r35 status okay，CVM p3767 无 vic 覆盖）。

### 2.2 CCPLEX 可访问性（documented + 强推断，**未上机实测**）
- mainline `drivers/gpu/drm/tegra/vic.c` 跑 CCPLEX 直接 ioremap 0x15340000 + boot falcon。
- L4T nvvidconv/nvcompositor/VPI（VIC backend）经 `/dev/nvhost-vic` 驱动。
- host1x `nvidia,vmid=1`（VM1=CCPLEX），VIC 无 firewall / DCE-owned 属性（对比 NVDisplay priv CCPLEX 访问 SError 已测）。
- **第一步验证**：BPMP 使能 CLK_VIC / 解 RESET_VIC / 上电 PG_VIC 后，读 0x15340000 falcon `CPUCTL(0x1100)` / `IDLESTATE(0x104c)`，不 SError 即确认 CCPLEX 可达。

### 2.3 固件：明文 + 可自由分发 + 无安全启动链（已测 hexdump）
- linux-firmware **`nvidia/tegra234/vic.bin` 24064B**：bin-header-v1 magic **0x10fe** ver1 size0x5e00 os_hdr_off0x100 os_data_off0x200，off0x1c=**0xa5a5a5a5**（新 fw 标记，支持 context isolation 免 FCE ucode），**0x400 起明文 falcon 指令**（`00fe 9300 f800 890c...`，非加密）。
- WHENCE：Driver tegra-vic，l4t-r39.2.1，**Licence redistributable（LICENCE.nvidia）** → 可打包进 ramfs。
- **boot 流程**：等 DMACTL(0x110c) scrub → DMATRFBASE(0x1110)=(iova+data_off)>>8 → 256B chunks 经 DMATRFCMD(0x1118) DMA 到 IMEM/DMEM → IRQMSET/IRQDEST/ITFEN → BOOTVEC(0x1104)=0 / CPUCTL(0x1100)=STARTCPU → 等 IDLESTATE==0。
- **全程无 ACR/WPR2/TSEC/GSP 签名校验**（与 GA10B GR 签名+加密鲜明对比）。T234 VIC 非 RISCV 型（has_riscv 仅 T264）。

### 2.4 能力 → g2d 映射（documented VPI 矩阵 + TRM 引文）
| g2d op | VIC | 备注 |
|---|---|---|
| fill | ✅ | |
| blit | ✅ | 含 CSC YUV↔RGB |
| blit_alpha | ✅ | |
| rotate 90/180/270 | ✅ | +flip，TRM 列任意角 |
| scale | ✅ | nearest/bilinear/bicubic polyphase |
| **gaussian_blur** | **❌** | VPI 明确 Gaussian Filter VIC=no（仅 CPU/CUDA/PVA）→ **必须 NEON 回退** |
| blit_to_phy / rotate_to_phy | ✅（推断） | SMMU bypass 直写线性 FB 物理地址 |

格式：不支持 24bit packed RGB，用 **32bit xRGB/ARGB8888**（EwokOS FB 已 32bpp，无冲突）；输出 pitch-linear / block-linear 皆可。

### 2.5 host1x 编程模型（documented，L4T 源码提取）
- channel regs（hw_host1x08_vm.h）：DMASTART0x00 / DMAPUT0x08 / DMAGET0x10 / DMAEND0x18 / DMACTRL0x20 / CMDFIFO_STAT0x24（EMPTY bit13）/ CHANNELSTAT0x38 / **SMMU_STREAMID0x84**。
- syncpt：SYNCPT(x)0x8080+4x / CPU_INCR(x)0x6400+4x / INT_THRESH(x)0xa088+4x。
- pushbuffer opcodes：setclass=(0<<28)|(off<<16)|(class<<6)|mask / incr=1<<28 / nonincr=2<<28 / imm=4<<28 / gather=6<<28 / **setstreamid=7<<28|sid**。
- host 方法：INCR_SYNCPT=0x0（cond<<10|indx）/ WAIT_SYNCPT=0x8；falcon 投递 UCLASS_METHOD_OFFSET0x40=method>>2 / DATA0x44。VIC MLOCK=HOST1X_COMMON_VIC_MLOCK 0x4078。

### 2.6 SMMU（推断，关键简化）
- NISO = ARM MMU-500（SMMUv2），**2 交错实例须同样编程**（arm-smmu-nvidia.c NUM_SMMU_INSTANCES=2）。
- S2CR 复位默认 **BYPASS**，tegra234 DTS 无 `arm,smmu-disable-bypass` → 给 SID0x34（VIC）/0x27+0x35（host1x）保持 S2CR=BYPASS 即用物理地址，**无需完整 SMMU 驱动**。需上机验 UEFI 交接后 S2CR 态。
- VIC 侧还写 THI_STREAMID0/1(0x30) + TFBIF_TRANSCFG(0x2044) SID 属性 + NV_PVIC_MISC_PRI_VIC_CG(0x16d0) 时钟门控。

### 2.7 工作量（推断）
最小 host1x + VIC blit 驱动 ~**1500-2500 行 C**：
- BPMP clk/reset/pg ~200（已有通道）
- host1x channel/syncpt/pushbuffer ~500
- falcon 加载 ~300
- VIC 方法 + g2d 对接 ~500-1000

### 2.8 最大风险 / 缺口（documented）
开源内核头 VIC class 0x5D **只暴露 2 个 FCE 方法**（VIC_SET_FCE_UCODE_SIZE 0x71C / OFFSET 0x72C），**真正 2D 操作方法号/参数布局不在任何开源代码**（L4T 用户态走闭源 libnvrm + nvhost_vic051.fw.desc）。需：
1. **Orin TRM VIC 章方法表**（NVIDIA developer 可下载，显示研究用过 TRM）——**关键 enabler**；
2. 或逆向 nvhost_vic 提交；
3. 或 mainline VIC uAPI 补丁。

无方法表只能搭框架（falcon boot + syncpt = 里程碑 1），2D op 编不了 → 可能把工期从周级拖到月级。

---

## 3. 3D 加速：GA10B port nvgpu

### 3.1 唯一 viable 开源驱动 = L4T nvgpu（documented）
- github.com/OE4T/linux-nvgpu，MIT 核心 + GPL 胶水，GA10B 支持 r34→r36.x，有 os/posix 层 + QNX/Horizon 移植 = 可移植；完整 GA10B 寄存器头 `include/nvgpu/hw/ga10b/hw_*_ga10b.h` = de-facto TRM。
- **open-gpu-kernel-modules：无 Tegra iGPU 支持**（discussion #576）→ 死路。
- **nouveau：无 GA10B**（platform 仅 gk20a/gm20b/gp10b）→ NVK/Mesa 也死。
- 无社区 GA10B bare-metal/RE 工作。

### 3.2 签名固件性质澄清（documented）——障碍是工作量，不是密码学不可能
整条安全链在 **rel-36 nvgpu 对 GA10B 完整实现**（nvgpu_init.c:933 finalize_poweron），**L4T 就用这份开源代码 + 官方 blob 点亮零售 Orin Nano**：
- ACR 跑在 **GSP RISC-V**（非老 falcon，bootstrap_owner=FALCON_ID_GSPLITE）→ WPR2 → PMU RTOS（RISC-V next core）→ FECS/GPCCS 加密 fw 经 fuse 内密钥 ACR 解密回写 WPR → 加密 netlist NETA/B/C/D → golden ctx。
- 签名验证在片上 BROM/fuse PKC，解密在 ACR 内部，**驱动全程不碰密钥，只搬运官方 blob + 复现握手** → port 到 EwokOS 不需破解任何东西。
- **障碍 = 移植工作量 + 必须用 NVIDIA 专有固件二进制。**

### 3.3 启动顺序（nvgpu_init.c，documented）
pg35 → clks → reset19 → detect → priv-ring → bus → fifo reset → FB/LTC/grmgr → falcon SW init → PMU early → ACR init → GMMU/mm → FIFO → GR alloc → ACR execute（GSP HS boot, WPR2）→ PMU RTOS boot → GR init（golden ctx, FECS/GPCCS via ACR）→ channels/runlists → pushbuffer。

### 3.4 CE-only 2D 里程碑（documented 架构 + 推断组合）
- CE 无 falcon，不依赖 GR/PMU（ce.c:34 nvgpu_ce_init_support 无 falcon 调用；PMU 可整体关 support_ls_pmu=false → pmu_rtos_init.c:64 lock_acquire 直接返 0；GSP scheduler 可裁，runlist.c:638 保留 rel-35 传统 KMD 直接提交）。
- **但零售 fuse（priv_sec_en≠0）仍必须先 ACR-GSP + WPR2**（nvgpu_init.c:953/977 acr_init 在 fifo/CE init:1049 之前无条件执行，NVGPU_SEC_PRIVSECURITY 门控）。CE-only 省 GR/FECS/GPCCS/netlist/golden-ctx/safety-sched，**省不掉 ACR**。
- 风险：no-PMU/no-NVS 裁剪配置无 L4T 先例，需硬件实验验 priv lockdown 是否允许 ACR 完成但不 bootstrap 任何 LS falcon 下 host/CE 工作。

### 3.5 固件分发 / 许可（documented）
- `/lib/firmware/nvidia/ga10b/`：acr-gsp.manifest/text/data.encrypt.bin.prod、gpmu_ucode_next_prod_image/desc.bin + pmu_pkc_prod_sig.bin、fecs/gpccs_encrypt_prod.bin + pkc_sig、NETA/B/C/D_img_prod_encrypted.bin（safety-scheduler 可裁）。
- 渠道 = L4T **nvidia-l4t-firmware** deb / Ubuntu **linux-firmware-nvidia-tegra**；**不在上游 kernel.org linux-firmware**（nvidia/ 有 tegra234 host1x fw + gk20a/gm20b/gp10b，无 ga10b）。
- NVIDIA 专有 Tegra_Software_License：hobby 自用低风险 / 公开再分发需读 EULA（**建议用户自取 deb 解包，不随仓库带 blob**）。

### 3.6 nvgpu OS-layer port（documented）
- 干净 OS 抽象缝，MIT 核心 + GPLv2 胶水，已跑 Linux/QNX/Nintendo Horizon。
- os/posix 去掉 286640 行 mock-registers 后**真实 OS 层 ≈ 5390 LOC**（需重写接口面）：mutex/spinlock(irqsave)/rwsem/cond/atomic/kthread/msleep-udelay/周期 timer/kzalloc-kfree/**物理连续 DMA + sgt**/firmware 加载/readl-writel/IRQ 注册/workqueue。
- **无需 SMMU**（hal_ga10b.c:2068 NVGPU_MM_BYPASSES_IOMMU=true，物理地址 DMA 内部 GMMU）。EwokOS bsp_bpmp 已通（已测）对接 clk/PG HAL。
- LOC：GA10B hal .c≈27139 + .h≈3180 + hw 头≈5825，common≈101757（大量 config 可裁 FuSa/debugger/recovery/MIG/压缩），os/linux≈41578（nvmap/字符设备/DRM 大部分可换 EwokOS 接口）。

### 3.7 里程碑估算（推断）
- OS 缝重写 + 裁剪构建 ≈ 6-10K LOC 新代码。
- **M0 CE-only 2D（含 ACR/WPR2，无 GR/PMU）≈ 2-4 人月**。
- **M1 完整 GR 链 + compute channel 再 +3-6 人月**。
- 主要风险在无先例配置的硬件调试，非代码量。

### 3.8 3D API 阶梯（推断）
NVIDIA GL/Vulkan/CUDA UMD 全专有，绑 nvgpu ioctl/nvmap + Linux，无法搬。
1. **CE LAUNCH_DMA 2D blit**（M0 后，接 g2d offload，最现实近期收益）。
2. **compute-shader 2D（Adreno 风格）**（GR 通后走 compute channel）：**关键——Mesa NAK（Rust MIT）NIR→SASS 编译器已覆盖 Turing/Ampere，GA10B=sm87 同族 ISA → 可复用开源 shader 编译器**，不必从零写；但 NAK 输出 pushbuffer/QMD 绑 nouveau ABI，需自写薄 UMD 塞 SASS+QMD 进 nvgpu channel，+2-4 人月，sm87 细节需验。
3. **full GL/Vulkan 不现实**（自写整套 state tracking/UMD = NVK 数十万行等价，人年级，不建议）。

**compute-2D 明显比 full GL/Vulkan 现实。**

### 3.9 无绕过路线（documented + 推断）
非安全路径仅 priv_sec_en=0 工程硅片有效，零售 fuse 不可逆；open-gpu-kernel-modules 无 Tegra iGPU；nouveau 无 GA10B（L4T 装 denylist-nouveau.conf，mainline Orin 无 GPU 加速）；无 GA10B bare-metal/开源 fw/fuse-bypass 社区项目。

---

## 4. EwokOS g2d 接口契约（2D 驱动目标，Explore 核实）

### 4.1 架构
- g2d = **共享 daemon** `system/gui/drivers/g2dd/g2dd.c`（2330 行，machine-independent）+ 每机器 **`bsp_g2d.c` 后端**（ABI 跨机器必须一致）。
- 参考 = `machines/qcom/8gen3/system/libs/bsp/src/bsp_g2d.c`（Adreno forwarder 到 libadreno A2D+compute，ADRENO_ENABLE=1 门控，version 返 690）。
- **Orin 无 bsp_g2d.c yet。**

### 4.2 17 命令（g2dclient.h:13-42）
FILL_RECT0 / BLIT1 / BLIT_ALPHA2 / ROTATE3 / SCALE_TO4 / GET_CLOCK5 / **BLIT_TO_PHY6** / GAUSSIAN_BLUR7 / **ROTATE_TO_PHY8** / FILL_TRIANGLE9 / PGL_CLEAR10 / DRAW11 / PROGRAM12（3D 可选）/ GET_VERSION13 / CL_PROGRAM14 / DISPATCH15 / PROGRAM_FREE16（CL 可选）。
仅 **G2D_FMT_ARGB8888=0**（packed pitch=w*4，64B 对齐）。

### 4.3 结果码
G2D_OK0 / FAILED-1（可 CPU 重放）/ **NOT_SUPPORTED-2（sticky，client 永久关断该 op）** / TIMEOUT-3 / MMU_FAULT-4（提交后永不重放）。

### 4.4 后端 ABI（bsp_g2d.h）
init / clock_hz（可返 0）/ version（0 或 T234 码；graph.c:232 特判 690 要 16px 对齐 pitch）/ cmd / fill / fill_alpha / fill_triangle（可 -2）/ blt / blt_alpha / scale_to / rotate / rotated_size（forward arch_g2d_rotated_size）/ gaussian_blur（可 -2）/ **blt_phy + rotate_phy**（加速 displayd 扫描输出 flush 关键）。每 op 收 (argb_va, phy, contig, w, h, ...)。

### 4.5 消费侧
- graph 库（graph_g2d.c）路由 fill/blt/blt_alpha/rotate/scale/blur，gate = shm_contig && rect ≥ G2D_MIN_SIZE(128*128)，sticky per-op cap bits。
- displayd flush 先 g2d blit_to_phy，否则 memcpy 到 NC FB。
- xserverd 合成全走 graph_* 内部 g2d 路由（不直接调 g2dclient）。
- **核心合成器纯 2D**；3D = projects/{opengl portablegl, opencl, vulkan transpiler} 可选 offload，2D 驱动 stub bsp_g3d/bsp_cl 返 -2。

### 4.6 T234 2D 驱动落地步骤
1. 写 `bsp_g2d.c`（+ g3d/cl stub 返 -2）。
2. 加 g2dd 到 GUI_DRIVERS，链接 libbsp。
3. init.rd 起 `/drivers/orin.nano/g2dd /dev/g2d`（display clients 前）。
4. **上层零改动自动加速**（has_g2d 翻转，graph_new_shm 分配 contig canvas，sticky latch 处理 declined op）。
- 一致性测试 app：`system/xwin/apps/xg2dtest` + `system/gui/bin/g2dtest`（已在 Orin 镜像）。

---

## 5. 推荐路线图

1. **近期（2D，周级前提 = 拿到 TRM VIC 章）**：
   - 里程碑 1：BPMP 使能 VIC（CLK167/RESET113/PG29）→ 读 falcon CPUCTL/IDLESTATE 确认 CCPLEX 可达（不 SError）→ host1x channel + syncpt + 明文 vic.bin falcon boot。
   - 里程碑 2：拿到 class 0x5D 方法表 → 实现 fill/blit/blit_alpha/rotate/scale + blt_phy/rotate_phy → 写 bsp_g2d.c 接 g2dd → 上层自动加速。gaussian_blur 走 NEON 回退（返 -2）。
2. **长期（3D，可选，人月级）**：nvgpu OS 缝 port → M0 CE-only 2D（含 ACR/WPR2）→ M1 GR + compute → compute-2D 复用 Mesa NAK。固件用户自取 deb，不随仓库带 blob。
3. **不建议**：full GL/Vulkan 自写 UMD。

**关键 enabler：取 Orin TRM VIC 章方法表**（2D）；**读 priv_sec_en fuse + 复用 bsp_bpmp**（3D 起步）。

---

## 6. 参考来源
- OE4T/linux-nvgpu（rel-36，GA10B hal + os/posix + nvgpu_init.c）。
- mainline linux tegra234.dtsi（v6.0+ vic 节点）、drivers/gpu/drm/tegra/vic.c、arm-smmu-nvidia.c。
- linux-firmware `nvidia/tegra234/vic.bin`（明文，LICENCE.nvidia 可分发）；ga10b blob 来自 L4T nvidia-l4t-firmware deb / Ubuntu linux-firmware-nvidia-tegra（专有）。
- L4T nvhost_vic / VPI 能力矩阵；Orin TRM VIC 章（待取）。
- EwokOS 树：system/gui/drivers/g2dd、system/gui/libs/{displayd,graph}、machines/qcom/8gen3/system/libs/bsp/src/bsp_g2d.c。
- 临时 clone（可能已消失，需 re-clone）：/tmp/gpu-research/{linux-nvgpu,t23x-public-dts,headers}、/tmp/vic-extract/。