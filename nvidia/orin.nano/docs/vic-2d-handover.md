# Orin Nano VIC 2D 加速 — 移交文档 (2026-11-23 13:35 更新)

> **状态: M1 全过 (已测); M2 卡在 boot stub 二次加载, 根因锁定到 falcon 权限墙 (已测), 未解。**
> 背景/可行性/能力映射见 [gpu-accel-research.md](gpu-accel-research.md); 显示链见
> [display-4k-handover.md](display-4k-handover.md)。本文只记 VIC bring-up 的实测态与接手点。
>
> 标注约定: **已测** = 真板实测; **源码** = 开源代码核对; **推断** = 未验证推理。
>
> **2026-11-23 15:30 更新: M2 已通过并集成进 xwin 桌面 (已测)。§0/§2/§6 的"boot stub 不二次加载 /
> falcon 权限墙"结论已被固件分析 + 上板结果推翻, 真因是两处方法序列错误。当前结论以
> [§8](#8-根因更正与-m2桌面集成结果-2026-11-23) 为准, §0–§6 仅作历史记录。**

---

## 0. 一句话现状

VIC 上电/时钟/host1x channel/syncpt/falcon 加载与起跑全部跑通 (**M1 已测**)。
falcon 起跑后执行 vic.bin 里 1KB boot stub, stub **scrub 清空 IMEM 后从未发起 DMA 把主固件
(bin 0x600–0x4000) 装回 IMEM** → IMEM 全 0 → 任何 fill/blit/Execute 无 config 抓取、无中断、
无输出 (**M2 失败, 已测**)。stub 二次加载所需的 falcon 权限/时钟寄存器被硬件锁定, EwokOS
直接 MMIO 写不进去 (已测)。**软件加载/方法路径已逐项对齐 mainline/libdrm, 问题不在我们这侧。**

---

## 1. 决定性证据链 (已测, 2026-11-23 板上, `vicprobe mmio`)

用 falcon DMA 回读 (`DMATRFCMD.WRITE=1`, IMEM→FB) 直接读 IMEM, 把"加载失败"与"回读失败"分开:

| 时刻 | IMEM[0x000] | IMEM[0x400+] | 结论 |
|---|---|---|---|
| **STARTCPU 前** | `cf420049 95b60099` | — | 与 vic.bin stub 逐字一致 → **加载 DMA 正确, 回读机制正确** |
| **STARTCPU 后** | `00000000` | 全 0 (nz=0/64) | falcon 起跑后 **IMEM 被 scrub 清空** |

- post-boot `DMATRFMOFFS=0x300` (仍是我们最后一次回读留下的值), `FBOFFS=0x7900`, `cmd=0x1632`
  → **boot stub 没有发起任何一次 DMATRF 传输** (MOFFS/FBOFFS 未被 stub 改写)。
- IMEM scrub 走 `DMACTL` 硬件 scrub (不经过 DMATRF, 所以 MOFFS 不变), 这解释了"IMEM 被清但
  MOFFS 不动"。
- Execute 后: falcon 短暂唤醒 (`busy=1`, CPUCTL 0x20→0→0x20), 但 `IRQSTAT=0` (无 SWGEN/EXTERR/
  HALT/DMA), `tfbif rd=0 wr=0`, `dst(4,4)=a5a5a5a5` (sentinel 未变, want=ff112233), `dbg0=0xf6000`。
- arena 区与 fwbuf 区两组 fill 均失败 → **排除内存区域 firewall** (两块不同物理地址都一样)。

### 1.1 固件结构 (已测, vic.bin 24064B md5=8a4abfafa924ec38842d38b04392cc4f, 与 mainline 一致)

```
bin hdr: magic 0x10fe  ver 1  size 0x5e00  os_hdr_off 0x100  os_data_off 0x200  os_size 0x5c00
         hdr[6]=hdr[7]=0  (FCE embedded 标记, mainline 据此跳过显式 FCE 加载)
os hdr : code_off 0  code_size 0x400 (1KB boot stub)
         data_off 0x3e00  data_size 0xb00 (data 段全 0)
主固件指令填在 image[0x600..0x4000] (即 os blob[0x400..0x3e00]), 应由 stub 二次加载进 IMEM。
stub 首字: cf420049 95b60099 0094f101 ...   主固件首字(fw+0x600): 94bdf4bd 0f00f9cb
```

---

## 2. 权限墙 (已测) — 当前真正的阻塞

stub 二次加载需要的 falcon 权限/时钟寄存器, EwokOS 直接 MMIO 写大多被硬件挡掉 (写后读回不变):

| 寄存器 | offset | 写入 | 读回 | 结论 |
|---|---|---|---|---|
| DMA priv mask | 0x1190 | 0 | `0xffffffff`/`0xfffffcee` | **锁定** |
| IMEM priv mask | 0x1280 | 0 | `0xfffffcee` | **锁定** |
| CPUCTL priv mask | 0x1288 | 0 | `0xfffffcee` (默认 ffffffff) | **锁定** (但 STARTCPU 本身可写, ran=1) |
| MTHDCTX priv mask | 0x1294 | 0 | `0xfffffcee` | **锁定** |
| SCTL LSMODE/LSMODE_LEVEL | 0x1240 | 0x31 | `0x3000` (复位值) | **锁定** |
| MISC_PRI_VIC_CG | 0x16d0 | 0x40044 | 0 | **锁定** |
| THI STREAMID0 | 0x30 | 0x34 | 0 | **MMIO 挡**; 仅 host1x channel SETSTREAMID 可编程 (已做, 无效) |
| THI STREAMID1 | 0x34 | 0x34 | 0x34 | 可写 |
| TFBIF_TRANSCFG | 0x2044 | 0x22222222 | 0x22222222 | 可写 (写 0x8 反而读回 0, 部分字段受保护) |
| **PRIVSTATE.PRIV** | 0x1060 | 1 | **1** | **可写**, 但设 1 后 stub 仍不加载 |

priv mask 位义 (TRM): `WRITE_PROTECTION[7:4]` 0=ALL_LEVELS_DISABLED(全允许) / 8=ONLY_LEVEL3 /
15=ALL_LEVELS_ENABLED(全挡); 复位 0xffffffff。我们的 MMIO 能写 DMATRF/CPUCTL (说明在最高权限级),
但 stub 起跑后所在权限级被这些 mask 挡在 DMATRF 之外 (推断)。

**推断 (未验证)**: Linux 下这些 falcon 安全/权限态由 UEFI/BPMP 上电序列建立, stub 得以在足够
权限级跑二次加载; EwokOS 走 BPMP pg/clk/reset + 直接 MMIO, 绕过了这套安全初始化, stub 被卡在
低权限级, scrub 后无法 DMA。**这是下一步主攻方向。**

---

## 3. 已排除的假设 (本轮已测, 别再重复)

- 内存区域 firewall: arena + fwbuf 两块不同物理地址都失败 → 排除。
- host1x 投递: MMIO 直投与 channel 投递现象一致 → 排除投递路径。
- STREAMID0=0: 用 channel SETSTREAMID 编程成 0x34 (已测读回 0x34), 仍不加载 → 排除。
- TRANSCFG 路由: 全 FALCON(0x22222222) 与 mainline ATT0=HW/ATT1=FALCON(0x8) 都试过; 0x8 时
  加载 DMA 反而 idle timeout (TRANSCFG 读回 0), 已回退 0x22222222 → 路由不是根因。
- STARTCPU 权限: ran=1, CPUCTL priv mask 默认 ffffffff, STARTCPU 确实执行 → 排除。
- SMMU: smmu_niso1 VIC SID0x34 / host1x SID0x27 CLIENTPD=1 全 bypass (已测 a=0x00250001
  b=0x00200001) → 排除翻译。
- cache: dma_alloc NOCACHE (mmu.h PTE_ATTR_SYS_DMA=NOCACHE) → 排除一致性。
- 整段 IMEM 加载实验: 把 code_size 当 0x3e00 全装 → falcon HALTED (CPUCTL=0x10), 已回退
  code_size=0x400。**IMEM≥16KB 确认 (HWCFG=0x80403810), 全装无 ERROR, 但 stub 不跑主码。**
- priv mask  lowering (写 0 到 0x118c/0x1190/0x1280/0x1284/0x1288/0x128c/0x1294/0x1298/0x13d0):
  读回 fffffcee, 且把原本正常的加载 DMA 搞成 idle timeout → **已回退, 别再降 mask**。

---

## 4. 代码地图 (全部未 commit)

核心文件 `system/libs/bsp/src/bsp_vic.c` (~1500 行, untracked):

| 符号 | 行 | 作用 |
|---|---|---|
| PROBE_SYNCPT=512 / PROBE_CHANNEL=62 / CLASS_VIC=0x5D | 40/41/130 | 探针常量 |
| VIC_THI_STREAMID0/1, MISC_PRI_VIC_CG, TFBIF_TRANSCFG | 44/45/48/49 | 0x30/0x34/0x16d0/0x2044 |
| `bsp_vic_power_on` | 221 | BPMP pg29/clk167+46/reset113 |
| `bsp_vic_regs_probe` | 258 | M1: CPUCTL/IDLESTATE/DMACTL 读 |
| `bsp_vic_host1x_init` | 302 | HV VM 表 + syncpt prot + SMMU bypass |
| `bsp_vic_syncpt_test` | 353 | syncpt512 CPU_INCR 往返 |
| `bsp_vic_channel_test` | 479 | host1x-class pushbuffer + IMMEDIATE syncpt |
| `falcon_chunk` | 513 | 加载 DMA (FB→IMEM/DMEM, WRITE=0, DMACTX1, 256B) |
| `falcon_read_chunk` | 533 | **回读 DMA (IMEM→FB, WRITE=1 bit5)** — 本轮新增诊断 |
| `falcon_wait_idle` | 556 | IDLESTATE 轮询 |
| `vic_channel_streamid0` | 575 | channel SETSTREAMID 编程 STREAMID0 (works) |
| `bsp_vic_falcon_boot` | 599 | reset→TRANSCFG/STREAMID/CG→scrub wait→加载→priv 探针→STARTCPU→回读诊断 |
| TRANSCFG 写 0x22222222 | 673 | 当前值 (mainline 是 0x8, 见 §3) |
| `bsp_vic_2d_can_rotate` / `bsp_vic_2d_enable` | 1150 / 1378 | M2 自检入口 (fill/blit/rotate) |
| `mmio_method` / `mmio_fill_region` / `bsp_vic_mmio_fill_probe` | 1412 / 1422 / 1484 | MMIO 直投 fill 探针 (arena+fwbuf 两区) |

**本轮新增的临时诊断 (接手后视情况清理)**: `falcon_read_chunk` + boot 里 pre-start/post-boot
IMEM 回读 klog + PRIVSTATE/SCTL/HSCTL 探针 klog。这些是定位用, M2 通过后应删。

其它:
- `system/libs/bsp/include/bsp/bsp_vic.h`: 含 `bsp_vic_mmio_fill_probe` 声明。
- `system/drivers/vicprobe/vicprobe.c`: 模式 `power|regs|host1x|syncpt|channel|boot [fw]|method|mmio|2d|all`。
- `system/firmware/vic.bin`: 24064B, 打包进 bring-up ramfs `/drivers/orin.nano/firmware/vic.bin`。
- `system/etc/xwin/init.rd:18`: g2dd 仍注释 (`@/bin/ipcserv /drivers/orin.nano/g2dd /dev/g2d`);
  `/tmp/vic2d.cfg` 设 `VIC2D_ENABLE=1`。**M2 未过, 不要启用 g2dd。**

---

## 5. 测试台与复现 (已测可用)

```bash
# 构建 (仅用户态改动, kernel 不动)
make -C machines/nvidia/orin.nano/system
make -C machines/nvidia/orin.nano/efi initramfs

# 上机 (board-pxe bridge TCP 8899 @ cafe; 串口 /dev/cu.usbmodem1101, 重枚举自动重连)
python3 machines/nvidia/orin.nano/test/autoboot.py reset
python3 machines/nvidia/orin.nano/test/autoboot.py boot --timeout 420
python3 machines/nvidia/orin.nano/test/autoboot.py login
python3 /tmp/fancmd.py "/drivers/orin.nano/vicprobe mmio" 8     # 设 Pico tx_delay_us+bulk send 防吞字, 看 IMEM 回读
# 分级: vicprobe regs / host1x / syncpt / channel / boot / method / mmio / 2d
```

- PXE: en7 USB LAN 192.168.8.1; `autoboot.py pxe_status/pxe_control/pico_reset` 可查/控。
- reset 失败 "cannot login to issue reboot" = 板子在 boot 中途, 等登录提示再 reset。
- **禁触**: `efi/main.c` (并行会话 WIP), `rollback-4k-20261123/` (并行会话产物)。

---

## 6. 接手下一步 (按优先级)

1. **查 UEFI/BPMP 交接态 (主攻, 推断真因)**: 在 EwokOS `bsp_bpmp_reset_assert(113)` **之前**
   快照 VIC priv mask(0x1190/0x1280/0x1288/0x1294)、SCTL(0x1240)、CG(0x16d0)、PRIVSTATE(0x1060)、
   HSCTL(0x10e4)、TRANSCFG(0x2044)。对比 Linux 依赖哪些被 BPMP/UEFI 预设的态。若 UEFI 已把
   stub 权限级配好, EwokOS 的 reset 可能把它打回复位默认 (全挡) → 考虑**不 assert reset**, 或
   reset 后由 BPMP 侧重建权限态 (查 BPMP 是否有 VIC falcon priv/secure MRQ)。
2. **BPMP 侧 VIC 初始化**: 确认 pg_on/clk/reset 之外是否还需 BPMP 做 falcon secure/priv 初始化
   (类比 NVDisplay 由 DCE/BPMP 主控)。grep L4T nvhost-vic / BPMP VIC MRQ。
3. 若 1/2 无果: 分析 stub (1KB, IMEM[0..0x400] 可回读) 看它二次加载的门控条件 (读哪个 scratch/
   寄存器决定 load vs halt), 再针对性满足。
4. **M2 通过后**: 删 §4 临时诊断 → `vicprobe 2d` 像素自检 → 启用 g2dd (init.rd:18 去注释,
   VIC2D_ENABLE=1) → g2dtest/xg2dtest + 用户目视确认桌面加速 → 恢复 X(4K 桌面)镜像。

**当前板子停在 bring-up 镜像 (已登录)。用户尚未在"继续查 BPMP 快照"与"先恢复桌面镜像"间选定。**

---

## 7. 参考来源

- mainline `drivers/gpu/drm/tegra/vic.c` / `falcon.c` (本地副本 `/tmp/vic-ref/`): falcon_boot
  加载 code+data, DMACTX1, 256B chunk, TRANSCFG ATT0=HW/ATT1=FALCON, CG=0x40044。
- libdrm `tests/tegra/vic42.c` + `vic.h` (本地 `/tmp/vic42.c`, `/tmp/vic-ref/`): T234 方法编码,
  VIC_PUSH_BUFFER reloc shift=8, Execute=1<<8, ControlParams=(2960/16)<<16, 7 方法无 Hist/Status。
- Orin TRM VIC 章 (本地 `/tmp/gpu-research/vic-chapter.txt`): DMATRFCMD 位义 @8061 (WRITE bit5,
  IMEM bit4, SIZE 10:8, CTXDMA 14:12), priv mask 布局 @8813, HSCTL@7731/SCTL@10198/PRIVSTATE@6783。
- [cyndis/vaapi-tegra-driver](https://github.com/cyndis/vaapi-tegra-driver) — VIC 用户态参考。
- [host1x opcode rewrite patch (linux-kernel)](https://lists.openwall.net/linux-kernel/2022/06/22/692)。
- [drm/tegra: falcon RISC-V external boot (dri-devel, T264)](https://lists.freedesktop.org/archives/dri-devel/2026-June/573566.html)
  — 确认 T234 VIC 非 RISC-V 型, 走传统 falcon 加载。

---

## 8. 根因更正与 M2/桌面集成结果 (2026-11-23)

### 8.1 旧结论为何不成立 (源码: vic.bin 代码分析, falcon fuc5 指令集参考)
- "1KB 段是 boot stub, 需二次加载主固件": 该段是常驻 OS 内核 (主循环 0x2c6、方法分发 0xacb、IRQ 0xf1)。
  app 代码/表在 Execute 时由 falcon 自己 `xcld/xdld` 按需装入, **不经过主机侧 DMATRF 寄存器**,
  所以 "DMATRFMOFFS 不变 / 启动后 IMEM 回读全 0" 不能说明没加载。
- "priv mask 全挡": TRM 语义是 15=ALL_LEVELS_ENABLED (复位值, 全开), 读反了。
- "mainline TRANSCFG=0x8": 实为 `ATT(1,FALCON=2)|ATT(0,HW=0)` = 0x20。

### 8.2 真因 (两处, 各自独立致命; 修复后 M2 一次通过, 已测)
OS 状态机: 空闲态收到 SetApplicationID(0x200) 进入 app 态; app 态里再次 SetApplicationID 或遇到未知方法,
都会 **丢弃后续全部方法直到 Execute, 然后以状态码 0x27 / 0x26 空转收尾** (syncpt 照常退休, 不读不写)。
1. 旧 `bsp_vic_falcon_boot` 末尾 MMIO 发了一次 SetApplicationID, `method_test` 经 channel 又发一次。
2. 每个 job 都带 `0x714 SetHistOffset`; 本固件 app1 的方法表没有 0x714。

修复: 启动还原为 mainline 序列 (无 PRIVSTATE/SCTL 写), 存活检查改发 Nop(0x100); job 去掉 0x714/0x738,
新增 `0x830` notifier, 退休后读固件状态字 (0=成功, 状态码表见 `bsp_vic.c`)。

### 8.3 上板实测 (已测)
| 项 | 结果 |
|---|---|
| M1 (新启动序列) | PASS |
| fill | 像素精确, fmt=32, chroma=16383 |
| 1:1 copy (含 alpha 字节) | 精确: AlphaFillMode=COMPOSITED(4) + ConstantAlpha=0 |
| 仅颜色精确 copy (alpha 写 ff) | AlphaFillMode=SOURCE_STREAM(3) + ConstantAlpha=1; scan-out 推送用 |
| 90/180/270 旋转 | 精确 |
| 奇数矩形 / 高度非 4 行倍数 | 精确 |
| 源 pitch 非 256B 对齐 | **不支持** (pitch 320 的源被按 256 取); 宽度非 64px 倍数的画布走 NEON |
| TRANSCFG 0x22222222 | config/surface DMA 可用 |
| VIC 写 scan-out FB (4K carveout) | fill/copy CPU 回读 0 错 |
| g2dtest (256M slab, 画布 contig, VIC job: fill 2479 / blt 9963 / rotate 4838) | fill/blit/rotate 数据校验全 PASS |
| g2dtest 剩余 FAIL | `scale_br`、`scale_to_data`、`gaussian_blur_r6`: 纯 NEON 路径, 关 VIC 同样失败, 属共享 arch 引擎既有问题 |

### 8.4 性能 (已测) — 决定了哪些 op 走 VIC
- VIC job 固定开销 ~150us(fill)/~260us(copy), 之后 ~0.6ns/px; 引擎和 CPU 一样受内存带宽限制。
- **fill: 任何尺寸都比 CPU 慢** (1024²: 652 vs 308us) → fill 不走 VIC。
- 1:1 blit (uncached contig 画布): 256² 309 vs 245us, 384² 363 vs 544us, 1024² 918 vs 3863us → 门槛 320×320。
- 4K 整帧 scan-out 推送: VIC 4.9ms; CPU 从 uncached 画布 62.7ms; CPU 从普通 cached 画布 5.1ms。
- contig 画布是 NOCACHE 映射, NEON 读它很慢: 1080p blit_alpha 27ms (cached 3.1ms), fill_alpha 35ms (cached 4.4ms)。
- 结论: 启用 g2dd 后显示画布变 uncached, **必须有 VIC 才能把整帧推送拉回 ~5ms** (纯软件 g2dd 是 63ms);
  与不启 g2dd 的纯 CPU 桌面相比, 推送耗时持平 (4.9 vs 5.1ms), 收益是 CPU 让出与旋转输出, 不是帧时间。
  `shm_contig_size` 保持 64M: slab 越大, 落到 uncached 的窗口画布越多, alpha 合成越慢。

### 8.5 代码现状 (全部未 commit)
- `bsp_vic.c/.h`: 启动 + job + 自检 (每次启动实测并锁存 copy 模式/旋转/奇数矩形/任意高度) + reject 计数自动关断。
- `bsp_g2d.c`: NEON 兜底 + VIC (≥320×320 的 1:1 blit、直角旋转、blt_phy/rotate_phy); `devcmd /dev/g2d stat` 带 VIC job 计数。
- `vicprobe`: `regs|host1x|syncpt|channel|boot|method|2d|fb|bench`; `mmio` 模式与 TRANSCFG 调试口已删。
  **g2dd 运行时不要跑 boot/method/2d/fb/bench (会复位 falcon)。**
- `etc/xwin/init.rd`: displaymand 之前 `@export VIC2D_ENABLE=1` + 启动 g2dd。
- `efi/`: kernel.bin = `kernel/kernel.bin` (含 VIC MMIO 白名单; `rollback-4k-20261123/kernel.bin` **不含**, 用它 vicprobe 会 `SYS_MEM_MAP failed`);
  initramfs.img = X=1 桌面镜像 (g2dd + VIC)。

### 8.5a 桌面只显示左上角 (2026-11-23 15:40, 已测已修)
- 现象: 当前显示器 EDID 首选 4K60 pclk=533MHz > disp clk 上限 388MHz, `pick_mode` 自适应选了 1920x1080;
  window 0 的 armed size = 0x04380780 (1920x1080), 但 fbdisplayd 仍按 pd.fb 上报 3840x2160 → 只看到左上 1/4。
- 修复 (`fbdisplayd.c` init): `bsp_fb_init` 后读 `WIN_ARMED(0)+0x224`, 按实际生效的窗口尺寸上报
  width/height (pitch 仍 15360)。启动日志: `fbdisplayd: scan-out window 1920x1080 inside 3840x2160 fb`。
- 1080p 下 VIC scan-out 推送均值 1.5ms。但 64M slab 此时装得下窗口画布, alpha 合成落到 uncached 画布:
  `fill_alpha` 均值 8.4ms、`blit_alpha` 3.6ms, g2dd 占 CPU ~19% (§8.4 的代价在 1080p 下显现)。
- 该显示器在 388MHz 内分辨率最高的时序是扩展块 DTD 2560x1440@60 (241.7MHz); `pick_mode` 目前只看
  基础块 DTD + CTA VIC, 所以落到 1080p。未改 (dceprobe.c 属显示会话)。

### 8.6 未做 / 后续
- 目视确认桌面画面 (无相机, 需人看)。
- 让 VIC 真正提速需要 cached 画布 + 推送前 cache 回写 (内核 contig shm 策略), 或把 VIC 用在 CPU 不擅长的
  缩放/alpha 合成/YUV 上 (混合器与多相缩放器的像素数学尚未对齐 arch 引擎)。
- 窗口画布按 64px 对齐 pitch 才能作 VIC 源 (graph 库分配策略)。
