# Orin Nano 像素级 4K 显示 — 移交文档 (2026-11-23 08:40 更新)

> **状态: 4K 桌面已通 (已测 + 用户目视确认测试卡)。** 开机 → `dceprobe admin query chan mode hold`
> → 3840x2160@25 (CTA VIC 94, 297MHz, DP HBR2×4) → fbdisplayd/xserverd/misawm 渲染到同一块 4K FB。
> §1 链尾的"阻塞点 1/2"和 §1-11~18 的 pclk 推断已被 §0 取代, 保留仅作历史。

## 0. 两个阻塞的根因与修复 (2026-11-23 08:05–08:40, 全部已测)

1. **像素时钟不换 = UEFI 在 `pre_sor0_ref` (clk 128) 上留的 CCPLEX enable 引用。**
   - 证据: modeset kick 的同一时刻 BPMP 串口打印 `vpll0: set_rate when enabled` → 固件确实走到了
     设 pclk, 是被 BPMP 拒绝。BPMP debugfs (`/bin/bp tree vpll0`) 显示关 head + 释放旧列表后
     `vpll0 state=1 ref=1`, 唯一持有者 `pre_sor0_ref state=1 mrq=1(0x2)` (mask 0x2 = CCPLEX)。
   - `/bin/bp dis 128` 后 `vpll0 state=0`, `set_rate(449, 297M)` 立即成功。
   - 修复: `clk_release_uefi_refs()` 释放 VPLL0 整个子树的 CCPLEX 引用
     `467 452 86 126 128 132 125 451 449` (叶子在前); CPU 侧不再 set_rate, 由固件自己设。
   - 结果: `clk[post-modeset]` 449/86/452=297.000M, RG0 由 DCE 主控持有 (`mrq=1(0x8000)`),
     head ≈25fps, DPCD 0x202-0x205 = `77 77 01 01` (4 lane 锁定, 接收端口同步)。
   - 注意: MRQ_CLK IS_ENABLED 只反映**本主控**的引用, 看不到全局状态; 全局状态只能读 BPMP debugfs
     (`refcount` / `mrqcount` / `state`)。§1-13 "bInit=0 证伪" 的现象同源 (当时也没释放 128)。
2. **window0 UPDATE 不 latch = 硬件状态检查失败, 固件跳过了 UPDATE。**
   - 证据: 通道异常寄存器 (nouveau gv100 布局, T234 上存在, 已测可读):
     `NV_PDISP 0x611020 + chid*12` = stat / data / code, stat[11:0]=method>>2; window i 的 chid=1+i。
     失败时 `stat=10000080 (mthd 0x200=UPDATE) code=0x2d`。
   - 原因: nvdisplay 3+ 的合成管线是 FP16, 非 FP16 surface 要么配 ILUT, 要么开**合成旁路**
     (nvkms-evo3.c EvoFlipC5Common / EvoBypassCompositionC5, 已复核)。UEFI 的 armed 状态实测就是旁路:
     `0x2EC=0x00010000 (BYPASS)`, `0x440/0x444 (ILUT)=0`, `0x2F4=1`。
   - 修复: window `SET_COMPOSITION_CONTROL(0x2EC) = 1<<16`, `0x2F0=0`, `0x2F4=1`, 不配 ILUT。
     core 侧补 `WINDOW_SET_MAX_INPUT_SCALE_FACTOR(0x100C)=1x`, usage bounds 加 `ILUT_ALLOWED`。
   - 结果: `exc ... code=0`, armed `size=08700f00 ctx=c1d00030`。
   - 试错记录 (错误码含义为推断): 0x2d = 非 FP16 且无 ILUT; 0x0f = 用了 ILUT 但 usage bounds 未允许;
     0x2e = ILUT 配置本身仍不合格 (未再追, 已改走旁路)。
3. **BPMP IVC 进程间竞争 (顺带修复)**: fand 每秒轮询与诊断工具并发使用同一条 1 帧 IVC 环, 慢请求
   (首次 MRQ_DEBUG 触发 BPMP "debugfs initialized") 期间互抢响应 → BPMP 报
   `IVC response channel for peer 1 not ready`, 通道失效, fand 进 100% 保护。
   修复: `bsp_bpmp.c` 用 keyed shm (`'BPMP'`) 里的 test-and-set 字串行化每次事务, 持有者死亡 3s 后可夺锁。
   所有链接 libbsp 且长期运行的进程都要重编 (已重编 fand/bpmpdiag/dceprobe)。

新增诊断工具 (`/bin/bp`): `tree <clk名>` (全局 state/refcount/mrqcount/rate/parent 子树),
`dbg <debugfs路径> [过滤]`, `dbgw <路径> <值>`, `en|dis <id>...`。dceprobe: `shut` (关 head+释放后停),
`pat` (FB 留测试卡: 1px 白框/四角红绿蓝白/1px 棋盘格+竖线+横线/彩条), 每次 flip 后打印 `exc[...]`。

待定/后续: (a) 刷新率 25Hz (EDID 合规) vs `hz30` (VIC 95, 不在该显示器 EDID 里), 需用户定;
(b) dceprobe 仍是探针形态, 应收敛成常驻显示驱动并去掉诊断输出; (c) modeset 失败时无回退
(loader 固定发布 4K FB, UEFI 1024x768 FB 不再被渲染); (d) `/tmp/dce4k.ready` 写的是 pd.fb 尺寸而非实际模式。

目标: 用户 4K(3840x2160) 显示器上像素 1:1 输出, 无缩放/拉伸。
现状: FB 1024x768(4:3) 被 NVDisplay 硬件非均匀上缩放(横3.75×/纵2.8125×) = 拉伸根因。
路线: **Route A = DCE IPC modeset**(用户选定, 授权自主推进"跑到正常启动4k显示为止")。

标注约定: **已测** = 真板实测; **源码** = 开源代码核对; **推断** = 未验证推理。

---

## 1. 当前进度 ( Route A 链 ) — 2026-11-23 02:10 更新

```
boot-cmd 序列 → ADMIN IVC → ADMIN RPC(v4) → IPC_CREATE(DISPRM+RM_NOTIFY)
→ IVC establish → SET_LOGGING(0x0A) → RM_BOOTSTRAP(0x15) → RM_BOOTSTRAP2(0x19)
→ DCE_RM_INIT(0xA8)=0 (回传 hInternalClient=0xc2000001)
→ ALLOC root(0x41) / device(0x80) / subdevice(0x2080) / display(0xc670) = 0
→ CONTROL GET_CAPS_V2=0 → ALLOC NV04_DISPLAY_COMMON(0x73)=0
→ NV0073 查询: supported=0x300 connected=0x100 active(head0)=0x100, DPCD/EDID 经 AUX 读出
→ INTERNAL GET_IP_VERSION=0x04020000 / GET_STATIC_INFO (2 head, winMask 0xf)
→ WRITE_INST_MEM=0 → CHANNEL_PUSHBUFFER=0 → ALLOC core 通道(0xc67d)=0
→ CHNSTATUS_CORE EFI_OPERATION(7)→IDLE(0xB), 写 PUT=0x10 → GET=0x10   ← 以上全部已测
→ ALLOC window0 通道(0xc67e)=0 (UNCONNECTED→IDLE) → DP_SET_MANUAL_DISPLAYPORT=0
→ SET_IMP_INIT_INFO=0 → ALLOC NVC372_DISPLAY_SW=0 → IS_MODE_POSSIBLE(4K25)=possible
→ core UPDATE 关 head (SOR0 owner=none, armed 生效, RG 停)                       ← 已测
→ DP_CTRL 重训练 4 lane (HBR 或 HBR2) result=0, DPCD 0x202/203=77 77 0x204=01     ← 已测
→ DP_CONFIG_STREAM=0 → core UPDATE modeset: armed raster/viewport/pclk/SOR 全部生效 ← 已测
→ ★ 阻塞点 1: **像素时钟没换** — RG 仍按 65MHz 跑 (4K25 raster 实测 ~5.5fps)
→ ★ 阻塞点 2: window0 的 UPDATE 不 latch (assembly 已是 4K/新 ctxdma, armed 仍是 UEFI 的)
```

### ★ 2026-11-23 结论 (推翻 11-22 23:26 的"pushbuffer 内存分配阻塞")
1. **内存类根本不走 fw (源码 r38.4 resource_list.h + 已测)**: 0x3e/0x71/ctxdma(0x2) 没有
   RS_FLAGS_ALLOC_RPC_TO_ALL, 只存在于 CPU 侧 kernel RM; 对 fw 发它们必然 0x56。fw 通过
   subdevice **内部控制**拿物理地址:
   - `INTERNAL_DISPLAY_GET_IP_VERSION` 0x20800a4b (4B)
   - `INTERNAL_DISPLAY_GET_STATIC_INFO` 0x20800a01 (36B)
   - `INTERNAL_DISPLAY_WRITE_INST_MEM` 0x20800a49 (24B: phys u64@0 size u64@8 addrSpace=1@16 cacheAttr=0@20)
   - `INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER` 0x20800a58 (56B: addressSpace=1@0 phys@8 limit@16
     cacheSnoop=1@24 hclass@28 channelInstance@32 valid=1@36 pbTargetAperture=3@40 channelPBSize=0@44)
   这些用 **hInternalClient(DCE_RM_INIT 响应 union+4) + 固定 hSubdevice 0xABCD2080** 发(已测 0)。
2. **GSP_RM_CONTROL 固定头是 40 字节不是 24 (源码 r38.4 g_rpc-structures.h + 已测)**:
   hClient@0 hObject@4 cmd@8 status@0xC paramsSize@0x10 rmapiRpcFlags@0x14 rmctrlFlags@0x18
   rmctrlAccessRight@0x1C reserved0(u64)@0x20 **params@0x28**。旧 24B 布局下 result 仍回 0 但
   参数错位 16B(早先 GET_CAPS_V2 "成功"是假象)。ALLOC 布局不变(params@0x20)。
3. **DCE_RM_INIT 是 20B 结构**: bInit@0 maxDispClkRateDisppll@4 maxDispClkRateSppllClkouta@8
   maxHubClkRateSppllClkoutb@12 hInternalClient@16(r38.4 源码); fw r39 实际把内部 client 句柄
   回在 +4 (已测 0xc2000001)。时钟上限 Linux 取自 DT `nvidia,max-disp-clk-rate-khz`
   (UEFI 填), 目前发 0 (是否限制 pclk 未测)。**CPU 侧不负责设显示时钟**, fw 自己经 BPMP 配(源码)。
4. **NV_PDISP 寄存器要减 0x610000 (源码 kdispGetBaseOffset_v04_02 + 已测)**: 物理 = 0x13800000 +
   (NV_PDISP_x − 0x610000)。CHNCTL_CORE=+0x4e0, CHNCTL_WIN(i)=+0x4e4+4i, CHNSTATUS_CORE=+0x630,
   CHNSTATUS_WIN(i)=+0x664+4i (只有 i=0..3; **读 WIN(4)=+0x674 → EL3 RAS 死机, 已测**)。
   通道 control page: core=+0x70000 (PUT@0 GET@4, assembly 状态按方法偏移可读, armed=+0x78000),
   window i=+0x80000+i*0x1000 (armed +0x800)。**doorbell CPU 可写、无防火墙(已测)**。
   旧文档"sys_mem_map 部分映射 bug"是误判: 当时读的是未 rebase 的 +0x610630, 超出映射范围。
5. **head 时序不是 4K (已测, 推翻旧推断)**: core armed 状态 PIXEL_CLOCK=65MHz, RASTER=1344x806,
   VIEWPORT_IN/OUT=1024x768, SOR0 control=0x801(DP_A, owner head0)。即 UEFI 输出真 VESA
   1024x768@60, 拉伸是显示器自己做的 → 像素级 4K 必须**完整 modeset**。
6. **显示器/链路 (已测)**: displayId 0x100 = SOR0 DP_A (0x200 = 同 SOR 的 TMDS_A)。
   当前链路 **1 lane × HBR(2.7G)** (DP_GET_LINK_CONFIG + DPCD 0x100=0a 81), SOR 上限 HBR3
   (DP_GET_CAPS maxLinkRate=4)。sink DPCD: rev 1.4, max rate 0x1e(HBR3), 4 lane, TPS3/TPS4。
   EDID (AUX I2C 0x50, RM 的 GET_EDID_V2 对 DP 返回 0xffff): BKU270, 首选 DTD
   **3840x2160@60 CVT-RB, pclk 533.25MHz, htotal 4000 (hfp48 hs32 hbp80), vtotal 2222
   (vfp3 vs5 vbp54), H+ V−**。目标链路 HBR2×4 (17.28Gbps ≥ 12.8Gbps)。

7. **Orin Nano 做不了 4K60 (已测)**: 固件 DT `nvidia,max-disp-clk-rate-khz = <388000 385714>`,
   `nvidia,max-hub-clk-rate-khz = 540000` (EFI loader 启动时打印 `[uefi] disp clk max khz`)。
   dispclk 上限 388MHz < 4K60 的 533.25/594MHz (与 Orin Nano 规格 "DP 4K30" 一致)。
   该显示器 EDID 的 4K 低刷只列了 VIC 93/94 (24/25Hz), **没有 VIC 95 (30Hz)**; range limit 写 48-60Hz。
   dceprobe 目前按 EDID 选 VIC 94 = 3840x2160@25 (297MHz, htot 5280 vtot 2250); `hz30` 参数强制 VIC 95。
8. **fw r39 的结构体版本 ≈ NVIDIA 595.x (已测+源码)**: `rpc_dce_rm_init_v` = bInit@0 hInternalClient@4
   maxDispClkRateDisppll@8 maxDispClkRateSppllClkouta@12 maxHubClkRateSppllClkoutb@16 (kHz)。
   参考头用 open-gpu-kernel-modules tag 595.45.04 (/tmp/ogkm/595_45_04/); 用到的 0073/c372 控制
   结构体与 r38.4 逐字相同。
9. **nvkms 之外还必须补的 "kernel RM / DP lib 初始化" 调用 (全部已测 result=0)**:
   - `NV2080_CTRL_CMD_INTERNAL_DISPLAY_SET_IMP_INIT_INFO` 0x20800a54 (492B, 只填 num_dram_channels@4,
     现填 4=推断)。不发则 fw IMP 未初始化; 发后 DISPLAY_MODESET 事件才带真实带宽。
   - `NV0073_CTRL_CMD_DP_SET_MANUAL_DISPLAYPORT` 0x731365 (4B)。不发则 DP_CONFIG_STREAM 返回
     0x16 ILLEGAL_ACTION (已测), 发后 =0。
   - `NVC372_DISPLAY_SW` (0xc372, parent=device) + `IS_MODE_POSSIBLE` 0xc3720101 (2048B, 布局见
     dceprobe.c imp_check): 4K25 → possible=1 isoBw=2743000 floorBw=10649600。
   - DP_CTRL 0x731343 (28B: displayId@4 cmd@8=0x83 data@12=lanes|bw<<8 err@16 retryMs@20);
     DP_CONFIG_STREAM 0x731362 (84B, watermark 公式照 dp_watermark.cpp, 见 do_modeset)。
10. **RM_NOTIFY 事件 0x1011 = DISPLAY_MODESET** {bModesetStart, minRequiredIsoBandwidthKBPS,
    minRequiredFloorBandwidthKBPS} (Linux 用它调 ICC/EMC)。必须持续读走 (events_drain)。
    关 head: start(2743000, 0xffffffff)+end(0,0); 我们的 modeset: **只有 end(0,0), 没有 start**。
11. **时钟树实测 (CPU 侧 BPMP MRQ_CLK, `/bin/bp rate <id>...`)**, id: VPLL0=449 NVDISPLAY_P0=86
    RG0=452 RG0_M=467 DISP=85 HUB=84 SOR0_REF=126 SOR0_PLL_REF=125 SPPLL0_CLKOUTPN=439 CLKOUTA=440
    CLKOUTB=441 DISPPLL=454:
    - UEFI 态: VPLL0=P0=RG0=65M, DISP=388M(disppll), HUB=540M, SPPLL0_PN=270M。
    - 关 head 后: fw 把 RG0/RG0_M 切到 38.4M(osc) → **fw 确实自己经 BPMP 管时钟**。
    - modeset 后: DISP=300M(改挂 SPPLL0_CLKOUTA), HUB=48.2M → IMP 结果已下发; **但 VPLL0 仍 65M,
      RG0 mux 没切回** = head attach 的 pclk 步骤没做成。
    - CPU 侧 `set_rate(VPLL0, 297M)`: round 得 297M 但 set 失败, BPMP 串口打印
      **"vpll0: set_rate when enabled"**。UEFI 从 CCPLEX 通道留下了 452/86/126/125/449 各 1 个 enable
      引用 (Linux 靠 clk_disable_unused 清掉); dceprobe 已在关 head 后逐个 disable (CCPLEX 视角全 0),
      **但 BPMP 仍报 enabled** → 还有别的持有者 (推断: fw 自己在 UEFI 会话里留下的引用, 或 VPLL0 的
      其它子时钟)。这是当前最具体的线索。
12. **window 不 latch (已测)**: flip 方法进了 assembly (size/ctx/out 都是新值), loadv 计数在涨, armed 不变。
    推断与 11 同源 (head 未被 RM 视为 attach 成功, HUB=48M 即 IMP 认为无 window 负载);
    也可能是 instance memory/ctxdma 查表失败触发异常 (该路径至今没有独立验证)。

### ★ 2026-11-23 07:36 pclk 阻塞: bInit=0 证伪 + nvkms 源码调研

来源标注: **已测** = 本次板测; **源码·已复核** = 主会话亲自读过该行; **源码·未复核** = 仅 subagent
报告的文件:行号, 引用前须自行核对; **推断** = 未验证。

13. **§4 步骤 1d (DCE_RM_INIT bInit=0 释放 VPLL0) 证伪 (已测)**: `/bin/dp admin rminit deinit` =
    bootstrap → bInit=0 (result=0) → CCPLEX 逐个 disable 452/86/126/125/451/449 (is_enabled 全 0;
    451 本来就是 0 引用) → `set_rate(449, 297M)`: BPMP 仍打印 **"vpll0: set_rate when enabled"**,
    r=-1, now=64999958。即 VPLL0 的持有者既不是 CCPLEX 引用, 也不是 bInit=0 能释放的东西。
    (推断: fw 仍在运行的 head/RG 流水线自己持有。) 注意该测试里 head 未关, 与 §1-11 "关 head 后
    disable 再 set_rate 也失败" 合起来看, 两种状态下 CPU 侧都改不了 VPLL0。
14. **CPU 侧根本不编程像素时钟链 (源码·未复核)**: `nv_set_freq` 唯一调用点是 `RmEnableDeviceClks`
    (`src/nvidia/arch/nvalloc/unix/src/osinit.c:794-810`), 循环范围 GPU_FIRST..GPU_LAST (`nv.h:254-261`,
    只有 sysclk/nvdclk/gpc*clk), 不含 VPLL0/RG0/nvdisplay_p0。`nv-clk.c:55/81/90` 有这三个时钟的名字
    映射和 `nv_set_parent`(:663)/`nv_is_clk_enabled`(:784), 但两棵开源树里零调用者。CPU 只在
    DCE_RM_INIT(bInit=1) 时把 maxDispClkRate 传给 fw (`dce_client_rpc.c:672-682`)。
    → **推断**: VPLL0→p0→rg0 由 fw 在处理 core UPDATE 时自己经 BPMP disable→set_rate→enable
    (fw 闭源, 无代码可证)。结论: **放弃 "CPU 侧 set_rate(VPLL0)" 这条路**, 目标改为让 fw 的 attach
    路径真正走到 pclk 步骤。
15. **nvkms 的 pclk 方法块 (源码·已复核, `src/nvidia-modeset/src/nvkms-evo4.c:260-282`)**: 依次写
    FREQUENCY(0x200C)=pclkLo → FREQUENCY_HI(0x20C0)=pclkHi → CONFIGURATION(0x201C)=
    NOT_DRIVER_FALSE|HOPPING_DISABLE|HOPPING_MODE_VBLANK → FREQUENCY_MAX(0x2028)=pclkLo →
    FREQUENCY_HI_MAX(0x20C4)=pclkHi。偏移/取值见 `clc97d.h:701-749, 893-896` (已复核):
    三个 CONFIGURATION 字段的取值都是 0, 即 **0x201C=0, 与 dceprobe 现状 (do_modeset) 相同**。
    subagent 把 "CONFIGURATION 未写 NOT_DRIVER=FALSE" 列为头号嫌疑, **复核后不成立**。真实差异只剩
    dceprobe 没写 0x20C0/0x20C4 (HI 是 4bit 高位, 297M 时应为 0; 未写时 assembly 里是什么值未测)。
16. **nvkms 完整 modeset 顺序 (源码·未复核, `nvkms-modeset.c` KickoffProposedModeSetHwState :3171)**:
    - ShutDown (:3196→2530): audio off → `nvEvoDetachConnector` (`nvkms-evo.c:302`): BlankHeadEvo
      (清 cursor + 给所有 window flip 空 surface, :255-300) → SOR_SET_CONTROL owner=0
      (`nvkms-evo3.c:2366-2371`) → HeadSetDisplayId(0) (:342)。
    - PreUpdate: `nvEvoSetTimings` (写新 pclk) + `nvEvoAttachConnector` (SOR owner=head +
      DISPLAY_ID=activeRmId, :377-425)。
    - Kickoff (:2337): `nvDPPreSetMode` (闭源 DP 库 dpPreModeset, 下 link, `nvdp-connector.cpp:827-859`)
      → **一次** core UPDATE + kickoff + 等 notifier (`nvDoIMPUpdateEvo`, `nvkms-evo.c:523-536`)
      → `nvDPPostSetMode` (重训 link, :866-907)。
    - **与 dceprobe 的差异**: nvkms 的 detach 和 attach 在**同一个 core UPDATE** 里 armed; dceprobe 是
      "关 head UPDATE" + "modeset UPDATE" 两次。nvkms 在 ShutDown 里先给 window flip 空 surface,
      dceprobe 没做。无显式 RG stop / clk_disable / set_rate 步骤。
17. **WINDOW_SET_CONTROL owner=NONE 与 pclk 无关 (源码·未复核 + 推断)**: `EvoInitWindowMapping3`
    (`nvkms-evo3.c:1259-1285`, C9 hal 复用 `nvkms-evo4.c:1850`) 只在 modeset 首次 core update 时按
    RM 查询到的静态 `headForWindow` (`nvkms-rm.c:890`) 写 owner; 关 head 不改 owner, ShutDown 只 flip
    空 surface。→ §4 步骤 1c 里 "window owner 设 NONE" 这半条降级 (清 DISPLAY_ID 那半条已做)。
18. **UEFI 交接流程 dceprobe 没走 (源码·未复核 + 推断)**: Linux DP 库以 `headInFirmware=true` resume
    (`nvdp-connector.cpp:1023-1070`), 经 `enableLinkHandsOff/releaseLinkHandsOff` (:1225-1233) 交接;
    裸机未走, fw 可能仍认为 link/head 归 firmware。IMP 侧 `nvEvoPrePostIMPC3` 是 no-op
    (`nvkms-evo3.c:3494-3497`), 带宽走 `nv_imp_icc_set_bw` (`os.c:5005`), 与 VPLL 无关。
    `clk_disable_unused` 式的清引用机制在开源树里**找不到**。

dceprobe 本次改动 (未提交): 新增 `deinit` 参数 (bInit=1 前先发 bInit=0 + `clk_release_uefi_refs` 探针,
仅诊断用, 已证伪可删); `clk_release_uefi_refs` 的 disable 列表补上 451 (nvdisplay_p0_ref)。

**板子状态**: 每次测试后 `python3 dp_run.py "uname" 3` 复位即恢复 UEFI 1024x768 画面;
跑过 `mode` 而未复位时屏幕处于错误时钟的 4K raster (显示器大概率无信号)。

## 2. 核心工件

- 驱动: `machines/nvidia/orin.nano/system/drivers/dceprobe/dceprobe.c` (未提交, ramfs 别名 `/bin/dp`)
  - 已入 drivers/Makefile + efi/Makefile 双清单。
- 部署: `bash /tmp/deploy_dp.sh` (build dceprobe → cp → make efi initramfs → e2cp 校验 → echo img=md5)
- 板测(新): `cd /tmp && python3 dp_run.py "<cmd>" <等待秒> ...` (多命令; `HARD=1` 用 Pico 硬复位,
  `NOREBOOT=1` 不复位)。例: `python3 dp_run.py "/bin/dp admin query chan kick" 40 "/bin/dp mmio core" 8`
  - 只读探针: `/bin/dp mmio [core] [win]` (通道状态/armed 状态), `/bin/dp peek <hexoff> [n]`
    (逐个打印偏移后再读, 用于定位致命寄存器)。`query` = NV0073 查询 + DPCD/EDID。
- 板测(旧): `cd /tmp && python3 dp_test.py "/bin/dp admin chan"` (reset→boot→login→设 Pico tx_delay_us+bulk send→读日志)
  - **正确调用必须带 `admin`**: `/bin/dp admin <flags>`。`/bin/dp chan` 会落默认
    NVDisplay dump 路径读 0x610630 → translation fault 进程被杀(非崩溃, 是旧诊断路径)。
  - 模式递进: `info|ipc|disprm|boot|boot2|rminit|alloc|chan` (chan = alloc 全链 + 通道)。
- 参考源码(临时, 可能需重新获取):
  - **`/tmp/t234research/nvdisp-r38/` = OE4T/nv-kernel-display-driver 分支 l4t/l4t-r38.4 (580.00),
    结构体布局以它为准**; 540.4.0 那棵树布局已过时(control 头/通道 alloc 参数都不同)。
    结构体 sizeof/offsetof 用 `/tmp/r38off/off.c`, `off2.c` 对 SDK 头实编得到。
  - `/tmp/r38/` = OE4T/linux-nv-oot l4t/l4t-r38.4 DCE 驱动 (板 fw=r39.2.1, admin v4, r38.4 是最接近代理)
  - `/tmp/t234research/nv-kernel-display-driver/` = open RM 540.4.0 client + nvidia-modeset (EVO 参考)
  - SDK 头: `.../src/common/sdk/nvidia/inc/` (nvos.h, class/cl*.h, ctrl/*)
- 完整研究记录: `~/.qoder/projects/-Users-tony-work-ewokos/memory/project-orin-display-research.md`

## 3. 协议关键事实 (全部已测或源码证实, 勿再怀疑)

### 传输层
- 板 fw = L4T r39.2.1 / JetPack 7.2.1, **admin 接口 v4**: ADMIN_CMD_SIZE=**56**, RESP_SIZE=**1728**,
  args@frame+**12** (raw 帧=[u32 len][struct], .args 在 struct+8)。
- IVC 环: NFRAMES 槽位=count%nframes (tx 用 tx.count, rx 用 rx.count); EST=0/SYNC=1/ACK=2。
- AST 窗口 mask-based: **AST_IOVA % AST_LENGTH == 0** 必须。NC pool 只 32K 对齐 →
  过量分配后**对齐 PHY**(不是 VA! v_base/phy_base 低位不同), va=va0+(phy-phy0)。
  AST_LEN=REGION_SZ=131072。LOG_BUF(65536) 独立分配同样对齐 PHY。
- SET_LOGGING(0x0A) 是 RM_BOOTSTRAP **前置必需**(缺它 DCE_RM_INIT 时 fw trace 空指针崩溃,
  DFAR=0 已测)。args: flags=6@0, iova(u64)@8, buff_size@16, log_level=0xFFFFFFFF@20, stream_id=0x08@24。
- SMMU niso0 全局 bypass (CLIENTPD=1), SID 0x08 → **物理地址当 IOVA 直接用**。
- region 布局: admin[0,16640), DISPRM rx@16640/tx@20864 (nframes=1 fsize=4096 Q_SZ=4224),
  RM_NOTIFY rx@25088/tx@41600 (nframes=4 Q_SZ=16512)。
  mailbox: sm0=FROM_RM(0x160000) sm1=TO_RM(0x168000) sm2/3=RM_NOTIFY(0x170000/0x178000)
  sm4/5=ADMIN(0x180000/0x188000) sm6=BOOT_CMD(0x190000) sm7=IRQ(0x198000), 基 0xd800000。

### DISPRM RPC (已测 header/params 格式正确)
- rpc_message_header_v03_00 (32B): version=0x03000000@0, sig='VRPC'(0x43505256)@4,
  length=32+plen@8, function@0xC, rpc_result=0xFFFFFFFF(pending)@0x10, private@0x14, seq@0x18。
  帧=[u32 mlen][rpc msg]。响应原地覆写, result@+0x10 (0=NV_OK)。
- 功能号: GSP_RM_CONTROL=**0x4C**, GSP_RM_ALLOC=**0x67**, DCE_RM_INIT=**0xA8**(bInit=1, 1B)。
- alloc_v (union 内): hClient@0 hParent@4 hObject@8 hClass@0xC status@0x10 paramsSize@0x14
  flags@0x18 reserved@0x1C **params@0x20** (全帧 params@0x40)。
- control_v (**r38.4/r39, 已测**): hClient@0 hObject@4 cmd@8 status@0xC paramsSize@0x10
  rmapiRpcFlags@0x14 rmctrlFlags@0x18 rmctrlAccessRight@0x1C reserved0@0x20 **params@0x28**
  (全帧 params@0x4C)。旧版写的 params@0x18 是 540 布局, 错。
- **ALLOC 前必须先 DCE_RM_INIT**(否则 dispatch table null → prefetch abort PC=0x40000000, 已测)。
- 显示 RM 自包含于 DCE fw (RMCFG_FEATURE_DCE_CLIENT_RM=1), **无需 GSP-RM fw**。
- fw 崩溃可恢复: MB2 每次启动重载 DCE fw (已测多次)。

### ALLOC 链参数 (已测全 0)
- handles: hRoot=0xc1d00001 hDev=0xc1d00002 hSub=0xc1d00003 hDisp=0xc1d00004。
- root: class 0x41, hClient=hParent=hObject=hRoot, paramsSize=0。
- device: class 0x80, paramsSize=**56** (sizeof NV0080_ALLOC_PARAMETERS, 编译实测),
  **hClientShare=0** (不是 hRoot! 错则 err 59 INVALID_PARAMETER, 已测), 只设 deviceId=0。
- subdevice: class 0x2080, paramsSize=4, subDeviceId=0。
- display: class 0xc670, **paramsSize=0** (Linux nvkms-evo.c:4543 NULL params), parent=device。
- CONTROL 验证: NV5070_CTRL_CMD_SYSTEM_GET_CAPS_V2=**0x50700709** on hDisp,
  params{subdeviceIndex u32, capsTbl[1]}=8B, result=0 caps=0x24 (40B 头下已测)。
- 通道: NV50VAIO_CHANNELDMA_ALLOCATION_PARAMETERS r38.4 = **40B** (channelInstance@0
  hObjectBuffer@4 hObjectNotify@8 offset@12 pControl@16 flags@24 channelPBSize@28 subDeviceId@32),
  parent=hDisp, client=自己的 hRoot; pushbuffer 4KB。hObjectBuffer 只是 CPU 侧名字, fw 不查。
- NV0073 (hDC=0xc1d00005, parent=device, 无参数): GET_SUPPORTED 0x730107(12B)
  GET_CONNECT_STATE 0x730108(16B) GET_ACTIVE 0x73010c(16B) OR_GET_INFO 0x73028b(56B)
  DFP_GET_INFO 0x731140(16B) DP_GET_LINK_CONFIG 0x731360(24B) DP_GET_CAPS 0x731369(60B)
  DP_AUXCH_CTRL 0x731341(48B: displayId@4 cmd@12 addr@16 data[16]@20 size@36(入 n-1/出 n)
  replyType@40) — 全部已测 result=0。
- 显示 instance memory (CPU 维护的 64KB DRAM, 64K 对齐): hash 表 0..0x1FFF (1024 项 ×
  {handle, ctx=client[13:0] | (objoff>>5)<<14 | chn<<25}), 对象区 0x2000 起 (0x2000=NULL 实例,
  ctxdma 从 0x2020 起每 32B 一个: w0=0x5(PHYSICAL_NVM|RW, bit20 KIND PITCH=0) w1/w2=base>>8
  w3/w4=limit>>8)。通道号 core=0, window i=1+i。(源码 disp_inst_mem*.c; 写入代码已有, 待 window 实测)

## 4. 下一步 (按序)

见 §1 链尾。dceprobe.c helper: `rm_control/rm_alloc`, `chan_open`, `evo()/evo_kick()`,
`ctxdma_new/ctxdma_bind`, `aux_xfer/aux_dump/aux_edid`, `events_drain`, `imp_check`, `pick_mode`,
`dp_config_stream`, `core_wait_idle`, `head_tick`, `win_state`, `clk_report`, `clk_release_uefi_refs`,
`do_modeset`(完整序列)。

当前命令: `python3 dp_run.py "/bin/dp admin query chan mode" 80` (加 `hold` 保持 FB, `hz30` 强制 30Hz);
`/bin/dp tick` 只读看 head 帧率/window 状态; `/bin/bp rate <clk>...` / `/bin/bp setrate <clk> <hz>`。
判据: `head0 ... dpca` 每 100ms 行数 = pclk/htotal/10 (4K25 正确应 ≈5625, 现 ≈1232 = 65MHz)。

0. **(2026-11-23 07:36 修订, 优先于下面的 1a-1e)** CPU 侧 set_rate(VPLL0) 已放弃 (§1-13/14), 1d 已证伪,
   1c 的 window owner 半条降级 (§1-17)。新候选, 按成本:
   a. **单次 core UPDATE** 内同时 armed detach+attach (SOR owner、DISPLAY_ID、raster、pclk 全套),
      前置 DP link down, 不再拆成两次 UPDATE (§1-16);
   b. 补写 FREQUENCY_HI(0x20C0)=0 / FREQUENCY_HI_MAX(0x20C4)=0 (§1-15, 成本最低, 可与 a 合并);
   c. ShutDown 阶段先给 window0 flip 空 surface 再关 head (§1-16);
   d. 查 DP 库 headInFirmware 交接对应的 RM control (§1-18), 源码·未复核, 先核对再试;
   e. 原 1e: 解 fw 日志, 直接看 attach 路径在哪一步退出。
   判据不变: `clk[post-modeset]` 里 449/86/452=297M, `head0 ... dpca` ≈5625/100ms。
1. **pclk (旧候选, 1b 已做, 1d 已证伪)**: 找出谁还把 VPLL0 保持 enabled。候选(按成本):
   a. modeset 之后再做一次 CPU 侧 set_rate 探针 + 全时钟 dump, 看 fw attach 后是否自己 enable 了 VPLL0;
   b. 用 MRQ_CLK GET_ALL_INFO(14)/GET_PARENT(4) 列 VPLL0 的全部子时钟及 enable 状态;
   c. 关 head 时照 nvkms 把 HEAD_SET_DISPLAY_ID 清 0、window owner 设 NONE 再 UPDATE (目前只清了 SOR owner);
   d. 在 DCE_RM_INIT(bInit=1) 之前先发 bInit=0 让 fw 释放 UEFI 会话的全部状态/时钟引用 (屏会黑, 之后全量 modeset);
   e. 解 fw 日志 (SET_LOGGING 缓冲是二进制 trace, 头 {1, 0x28, 0, 0xffd8}, 需 display-t234-dce.bin 的串表)。
2. **window**: pclk 通了以后复测; 仍不 latch 则单独验证 instance memory (先用 core 通道的
   notifier/LUT ctxdma 这类不影响画面的句柄试查表)。
3. 画面确认(**必须请用户目视**: 1px 白框 + 1px 竖线/横线/棋盘格 + 彩条), 再把 modeset 并入常驻 fb 驱动
   (FB/instmem/pushbuffer 归常驻进程; clk 上限经 bootinfo→platform_data 传, 现为 dceprobe 内常量)。
4. 刷新率取舍需用户定: EDID 合规的 4K25, 或不在 EDID 里的 4K30 (`hz30`)。

## 5. 安全/环境约束 (必须遵守)

- 勿盲探 NVDisplay ≥0x6b0000 (EL3 SError) 或 DCE aperture +0 (EVP/R5 区, SError)。
  DCE HSP mailbox 区(≥0x150000) CPU 安全(已测)。fw 崩溃 MB2 重载可恢复, 但避免 hard-hang 探针。
- SD 卡只读(写需用户明确 OK); **不 commit 不 push 除非用户明确要求**;
  Orin 未提交改动与 qcom/rockchip 改动分开。
- 长串口命令防吞字 = MCP `pico_gpio set tx_delay_us` 抬高 Pico 固件 TX 字符间延时(默认 1000µs, 防溢出调到 20000µs)再 bulk `serial_log send`(dp_test.py/fancmd.py 已封装, 主机不做软件 sleep); Pico USB 掉线无软件恢复, 报告等物理重插, 勿轮询。
- klog 不是 slog。EwokOS dma_alloc(0,sz)/dma_phy_addr(0,va) = NC 物理连续池, VA↔phy 线性。
- 未提交 Orin 文件: dceprobe.c (+Makefile 链 libbsp), orin_net.h, hw_info_arch.c, efi/main.c
  (新增 fdt_prop_u32 + disp clk 打印), drivers/Makefile, efi/Makefile, bpmpdiag.c (rate/setrate),
  libs/bsp bsp_bpmp.c/.h (bsp_bpmp_clk_disable), 本文档。
- **NVDisplay 不存在的寄存器一读即死** (EL3 RAS "Error response from slave", TF-A 关核, 内核
  SError 吞不掉; 只能 Pico GP4 硬复位, `HARD=1`)。只读源码里确认存在的偏移; 4 个 window 之外
  的 CHNCTL/CHNSTATUS 不存在。
- (更正) "sys_mem_map 部分映射 bug" 不成立, 见 §1 结论 4。

## 6. 已废弃路线 (勿重走)

- GOP SetMode: MaxMode=4 全 BltOnly ≤1024x768, 无 4K (已测)。
- simplefb/EFIFB 线性 handoff: 此 UEFI BltOnly+FBPT block-linear 无 DCDT (已测)。
- CPU 直写 NVDisplay window priv: 防火墙 (已测)。
- KAPI: CPU 侧库非 fw 内服务, 不是捷径 (源码)。
- ALLOC_DISP_CHANNEL(17)/OBJECT(18) RPC: T19x 遗留, T234 走 GSP_RM_ALLOC (源码)。
- B1 provider Blt shim: 可行但只解 1024x768 上屏(已达成), 不解 4K modeset。