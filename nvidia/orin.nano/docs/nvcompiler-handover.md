# Orin Nano GA10B GPU 编译器 (nvcompiler) — 移交文档 (2026-11-25)

> **状态 (2026-11-25, 已测): 跑通。** 真实 `ocean` shader 在板上编译 (0.1 s) → 自研 NIR→sm_87 SASS 后端 → cubin → GA10B 执行, 3840x2160 全屏 18–23 fps (最初 11–13), 连续运行无 `warp_esr` / `sem timeout`。
> 板上加载行 `ga10b: kernel pgl_cs text=59808 fnv=19c39285 regs=57` 与 host 构建的 SASS 逐字节一致 (FNV-1a)。`g3dtest` 7/7 PASS (fresh GR 下, 以及 ocean 运行并退出之后)。
> **未做**: 板上画面只核对了天空采样点与 host 一致、整帧 hash 稳定 (§4.3), 海面需目视面板确认 (rig 无 FB 回读)。
>
> 驱动 / QMD launch / GR bring-up 见 [ga10b-3d-handover.md](ga10b-3d-handover.md); 背景见 [gpu-accel-research.md](gpu-accel-research.md)。
>
> 标注约定: **已测** = 真板或 host 实测; **源码** = 开源代码核对; **推断** = 未验证推理。

---

## 0. 一句话现状

共享 mesa GLSL→NIR 前端 → **自研 NIR→sm_87 SASS 后端** (`nv_backend.c`) → **SASS 编码器** (`nv_sass.c`) →
**cubin 合成** (`nv_cubin.c`) → 复用 `ga10b_dev_module` / QMD launch → GPU。

host 与真机均通过 (已测)。跑通过程中修掉的三个问题见 §4。

---

## 1. 架构与模块

代码在 `machines/nvidia/orin.nano/system/libs/nvcompiler/`。架构对齐 qcom/8gen3 的 `adrenocompiler`
(共享前端 + 机器侧后端), 但后端是**手写纯 C** (不移植 Mesa NAK Rust, EwokOS 无 Rust 工具链; 不用 nv50 codegen, 只到 Maxwell/Pascal)。

| 文件 | 职责 |
|---|---|
| `nv_sass.c/.h` | sm_87 128-bit SASS 指令编码器 (~30 指令: ALU/访存/控制流/转换/S2R) + 标签解析汇编器。逐位参考开源 Mesa NAK (MIT) `sm70_encode.rs` (ShaderModel70 覆盖 sm70–87), 对照 nvcc 13.5 sm_87 输出验证。 |
| `nv_backend.c/.h` | NIR→SASS 编译核心: NIR 标量化 lowering、全局 liveness + linear scan 寄存器分配、结构化控制流 (BCLEAR/BSSY/BSYNC 作用域)、ALU/intrinsic 发射、串行调度。 |
| `nv_cubin.c/.h` | 从 SASS + `nv_kernel_meta` 合成最小 cubin (CUDA ELF64), 复用 `projects/cuda` 的 `cubin.c` 解析器与 `ga10b_dev_module` 加载路径。 |
| `nv_pglc.cpp` | `pglcompiler.h` 契约编译半: 几何文本→预编译 twin 快路径; 其余→前端→`nv_be_compile_compute`→cubin。 |
| `nv_blob.c` | wire blob 序列化: `geom_kind!=0` 几何 twin (`ga10b_prog_blob_t`); `geom_kind==0` 全屏 compute (`ga10b_compute_blob_t` + cubin)。 |
| `nv_texlayout.c` | 纹理线性布局契约。 |
| `nv_meta.h` | 纯数据 `nv_kernel_meta` + **cb0 布局契约** (编译器↔daemon 共享, 见 §5.3)。 |
| `nv_pglc.h` | 后端私有 stage 快照 (`nv_pglc_stage`: kernel_id / meta / cubin)。 |
| `port/` | EwokOS mesa 构建 shim (`mesa_config.h` / `assert.h` / `syslog.h`), 与 adreno 同源 (backend-neutral)。 |

**共享前端**: `projects/opengl/libs/gles/pglcompiler` (vendored mesa GLSL→NIR, 所有 PGL_G3D 机器共用)。
`make.inc` 已置 `PGL_G3D_FRONTEND=yes` (原 no), `PGL_G3D_COMPILER_DIR` 指向本目录。

**旧 `nv_pglc.c` 已删** (拆成 `nv_pglc.cpp` + `nv_blob.c` + `nv_texlayout.c`)。

---

## 2. 已验证 (host, 全过)

测试基建在 `tests/` 与 `tests/host/`。`tests/host/Makefile.host` 是 host oracle: 用 native clang 编共享前端 + 本后端,
复用 adreno `shaders/Makefile.host` 模式 (`host_config.h` / `host_deps.mk` / `host_support.c` 提供 host glue)。

| 测试 | 内容 | 结果 |
|---|---|---|
| `test_sass_golden` | 15 条指令逐位对拍 nvcc 13.5 sm_87 vadd 金标准 | 15/15 (已测) |
| `test_asm` | 汇编器分支补丁 (前向/后向/未绑定) | 5/5 (已测) |
| `test_nir_build` / `test_nir_sass` | nir_builder 建核 → NIR→SASS | PASS |
| `test_cubin_synth` | 手构 SASS → cubin → `cubin_parse` 全字段反射 | PASS |
| `test_compile_cubin` | 真 `nv_be_compile_compute` → cubin → 解析 + nvdisasm | PASS |
| `test_control_flow` | if/else+phi、early-return、loop+break 三核 | PASS (nvdisasm 分支结构核对) |
| `test_ssbo` | load/store_ssbo + uniform 三核 | PASS |
| `compile_ocean` | 真实 ocean shader (`build_ocean_fs(2,3,4)`) → cubin, nvdisasm `.target sm_87` + `cubin_parse` | PASS (已测) |
| `emu` (`make -f tests/host/Makefile.host emu`) | **差分测试**: sm_87 warp 仿真器 (`nv_emu.c`, 独立位域解码) 跑后端产出的 SASS, 对拍 NIR 参考解释器 (`nir_ref.c`) | ocean (多种分辨率) + `shaders/{loops,flow,intops,floatops}.frag` 逐像素一致 (已测) |
| `check_disasm.py` | 仿真器反汇编 vs nvdisasm 逐条对比 | ocean 3738 条 / floatops 0 mismatch (已测) |
| `emu_ocean W H T samples` | 以板上 `ocean check` 相同的 16×9 采样网格打印参考画面, 并核对仿真器 == 参考 | 3840x2160 t=10 PASS (已测); 用法见 §4.3 |

运行: `make -f tests/host/Makefile.host emu EMU_ARGS="160 120"` (差分, 首选); `emu_ocean -f x.frag W H` 跑任意 shader, 产物在 `/tmp/nv-host/` (`*.emu.lst` 列表、`*_emu.ppm` 画面);
`make -f tests/host/Makefile.host ocean` (只编译); `make -C tests/host all` (单测套件);
`make -C tests` (金标准 + 汇编器)。nvdisasm 在 OrbStack debian VM (`~/cuda-nvcc-full/nvidia/cu13/bin/nvdisasm`,
`LD_LIBRARY_PATH=~/amd64-libs/usr/lib/x86_64-linux-gnu`)。

---

## 3. 编译器关键设计事实

### 3.1 SASS 编码 (已测, 勿信猜测)
128-bit/指令。opcode[0:12] (ALU 拆 [0:9]op + [9:12]form), guard pred[12:15]+inv[15], dst[16:24],
控制字 delay[105:109]/yld[109]/wr_bar[110:113]/rd_bar[113:116]/wt_mask[116:122]/reuse[122:126] (7=无 barrier)。
分支 rel = target-ip-4 (dword 单位) [34:82]。

关键 opcode: FADD=0x021 FMUL=0x020 FFMA=0x023 FMNMX=0x009 FSETP=0x00b IMAD=0x024 IMAD.WIDE=0x025
IADD3=0x010 IMNMX=0x017 ISETP=0x00c LOP3=0x012 SHF=0x019 SEL=0x007 MUFU=0x108 MOV=0x002 UMOV=0xc82
F2F=0x104(32→32)/0x110(64) F2I=0x105 I2F=0x106 LDG.E=0x981 STG.E=0x986 LDS=0x984 STS=0x988
LDC=0xb82 ULDC=0xab9 BRA=0x947 EXIT=0x94d NOP=0x918 BAR=0xb1d MEMBAR=0x992 BMOV=0x355/356
BSYNC=0x941 BREAK=0x942 BSSY=0x945 S2R=0x919 CS2R=0x805 R2UR=0x3c2 (sm<100; 0x2ca 是 Blackwell) WARPSYNC=0x148。

nvcc vs NAK 差异 (已踩坑): F2F.F32.F32(0x104 32→32) 是 no-op sm87 拒绝, 用 F2F.F32.F64(0x110);
LDG.E/STG.E 需 bit76=1(.E) + uniform base reg + size_bit72=1; MOV quad_lanes[72:76]=0xf; IMAD carry-in pred[87:90]=false;
@P0 BRA 条件在 guard[12:15] 非 [87:90]; **SEL/ALU src0 不支持立即数** (用 SEL dst,RZ,imm,!P 实现 0/1 选择)。

### 3.2 寄存器分配
- lowering 后全部是 32-bit 标量; 常量 (load_const/undef) 是操作数, 不占寄存器 (非零常量进不了立即数槽时物化到 scratch R0..R3)。
- 两遍编号 + liveness (循环外进入的值活到循环尾; phi 目的区间从最早前驱块尾开始) + linear scan, 无 spill。
- **cubin 声明的寄存器数 = 最高使用号 + 1 + 2**, 可用 R0..R252。旧后端少声明时报 `warp_esr` 错误码 `0xd` (OOR_REG), 改后消失 (已测)。"硬件占用最高 2 个 GPR" 这一机制是推断 (依据: 11 个 nvcc 核的声明规律 + NAK 源码)。ocean: 最高 R54, 声明 57。

### 3.3 控制流
结构化 walk `nir_if`/`nir_loop`, 做法对齐 NAK `nak_nir_lower_cf`:
- 每个作用域 `BCLEAR Bd; BSSY Bd, merge` … `merge: BSYNC Bd`; barrier 按嵌套深度取, 最多 16 层。
- if: `ISETP.NE.U32 P0, cond, RZ` + guard 形式 `@!P0 BRA`。
- loop = break 作用域 (进循环前 arm 一次) + continue 作用域 (每轮重新 arm)。
- break/continue: 把目标深度写入 escape 寄存器 (R4), 逐层经 BSYNC 退出, 每层 `ISETP.LT.U32 P0, esc, d; @P0 BRA 父 merge`。return/halt 直接 `EXIT`。
- phi: 前驱块尾发并行 copy, 环用 scratch 打破。
- 调度: 全串行 (每条等前一条的 barrier), 正确优先, 未做 scoreboard 优化 (实验结论见 §3.6)。

### 3.4 SSBO 寻址
`load_ssbo` / `store_ssbo` 的 block index 须为常量。**`.E` 访存指令隐含一个 UR 操作数 (nvdisasm 不打印), 必须装 `c[0x0][0x118]` (launch 时为 0), 与 nvcc 一致 (已测)**:
```
ULDC.64 UR4, c[0x0][0x118]                    ; 入口一次
MOV R0, offset ; MOV R1, 1
IMAD.WIDE.U32 R0, R0, R1, c[0x0][0x200+8*b]   ; R0:R1 = 基址 + 偏移
STG.E [R0.64+4i], Rdata
```
把缓冲区 VA 放进 UR、GPR 里只放偏移的写法, 硬件报 `warp_esr` 错误码 `0x10` (INVALID_ADDR_SPACE)。

### 3.5 cubin 合成 (已测)
`nv_cubin_build` 出最小 ELF64: `.text.<name>`(SASS, sh_info 高字节=regcount) / `.nv.info[.<name>]` /
`.nv.constant0.<name>` / `.symtab/.strtab/.shstrtab` / `.note.nv.tkinfo`+`.note.nv.cuinfo`。
**nvdisasm 门槛 (已测)**: `EI_OSABI=0x41` (ELFOSABI_CUDA, 非 0!); `e_flags=0x06000004|(sm<<8)` (abiversion 8);
section `sh_offset=64+body偏移`; 属性 fmt key=id<<8|fmt, fmt3=HVAL(u16)/fmt4=SVAL(u16 len+u32 elems)。

### 3.6 性能相关的代码生成 (2026-11-25)

**各类指令的 GPU 代价 (已测, 板上 4K 固定帧, 基线 launch 39.4 ms; 方法 = 把某一类指令每条再发一遍写到 scratch R3, 看 launch 增量; 画面 hash 不变)**:

| 类别 | 静态条数 | 加倍后 launch | 每条静态指令 |
|---|---|---|---|
| FADD/FMUL/FFMA | 2547 | 47.1 ms | ≈3.0 µs |
| MUFU (串行) | 490 | 62.8 ms | ≈48 µs |
| MUFU (副本不被等待, `wr_bar=2`) | 490 | 62.5 ms | ≈47 µs |
| FRND | 225 | 50.8 ms | ≈51 µs |

结论: MUFU / FRND 每条约等于 16 条普通 ALU, 且副本不等 barrier 也一样贵 → 受单元吞吐限制, 与调度无关 (后半句是推断)。
原 39.4 ms 的构成估计: MUFU ≈23 ms, FRND ≈11 ms, ALU ≈8 ms。**所以减 ALU 条数几乎不动帧率, 该减的是 MUFU/FRND。**

已做的代码生成优化:
- **操作数修饰折叠** (`mark_folds` / `alu_opnd`): fneg/fabs 折进 FADD/FMUL/FFMA 的源修饰位, 只用 nvcc 实际发过的形式 (FMUL 的 `|x|` 只在第一个源; FFMA 的取负只在第二/第三个源)。
- **NIR peephole** (`nv_peephole`): 去 `x+0` / `x*1`; `fadd/fsub + 单次使用的 fmul` → FFMA; `sin/cos(x)` 先显式乘 1/2π 再用 `*_normalized_2_pi`, 让同角的 sin 和 cos 经 CSE 共用这次乘法。板上: 4193 → 3407 条, launch 40.1 → 39.4 ms (已测, 印证上表)。
- **不走 FRND 的 floor** (`e_floor`): `FADD.RM t, x, 1.5·2^23; FADD t, t, -1.5·2^23`。|x| < 2^22 时 x+bias 落在 [2^23, 2^24), 该区间浮点数恰为整数, 向下舍入即 floor(x)+bias。范围外 (含 NaN) 用 `FSETP P0, |x| < 2^22` + `@!P0 FRND.FLOOR` 兜底 (条件不成立时不占舍入单元)。参数可证明有界时 (`src_bound`: sin/cos/fract/fsat 乘常数等) 连兜底都省掉。
  与 `floorf` 的差别只有 floor(-0.0) 得 +0.0 (host 上 2^32 个输入穷举, 已测); 指令带 `nir_fp_preserve_signed_zero` 时仍走 FRND。ocean: 225 条 FRND → 152 处 2 条 FADD + 73 处 4 条。
- **整数次幂** (`nv_peephole`): 前端把 pow 降成 `exp2(n·log2(|x|))`, n 为 1..128 的整数且乘法不超过 9 次时改成平方-乘。ocean: MUFU 490 → 450 (4.0 次幂 ×16, 2.0 ×2, 80, 60)。host 参考画面 144 个采样点最大差 1 LSB (已测)。
- 最后两项板上已测 (固定帧 launch): 基线 39.4 ms → floor 33.3 ms (`fnv=94ecaa55`, hash 仍为 `a04edbf8`, 14 次报告一致) → 加整数幂 31.3 ms (`fnv=19c39285`, hash `1b191378` 稳定; 与上一版 144 个采样点最大差 1 LSB; 天空两行与 host 逐字节相同)。
  floor 实际省 6.1 ms, 比按单价估的 11 ms 少: 带兜底的 73 处仍各占 4 条指令, 谓词不成立的 FRND 是否完全不占舍入单元没有单独测。

调度实验 (已测, 均在全串行方案上改 ALU 的 delay): delay 12 → launch 48.6 ms、画面不变; delay 4 或 1 → `warp_esr=051a000f` (misaligned_addr, pc 落在 STG: 地址还没算完就被读)。即固定延迟指令上的 barrier 不起保护作用, 靠的是 delay 本身; nvcc 输出里挖出来的延迟是真实约束:
GPR 写后读 4 拍 (读者是 FSETP/MOV/LDG/STG 为 5, I2F 为 6); 谓词→guard 13 (MUFU 14, BRA 操作数 15), 谓词→操作数 4; 变延迟指令 (LDG/MUFU/F2I/I2F/FRND/S2R) 必须用 write barrier, 等待者至少晚 2 拍。
按依赖关系逐条算 delay (不重排) 估计还能省 ALU 部分的 3 ms 左右 (推断), 未做。

---

## 4. 真机状态

### 4.1 已通 (已测, 2026-11-25)
- ocean → `pglc_compile_compute` → cubin → g2dd → `ga10b_dev_module` → launch → 画面, 3840x2160。帧率: 最初 11–13 fps → 现 18–23 fps (已测, 见 §4.4)。
- 几何 offload (预编译 nvcc 核) `g3dtest` 7/7 PASS。

### 4.2 跑通前的三个问题 (均已修)
| 现象 | 根因 | 修复 |
|---|---|---|
| `warp_esr=0x0005000d` | 错误码 `0xd` = OOR_REG: 声明寄存器数未含硬件占用的最高 2 个 | 后端重写, 声明 = 最高使用号 + 3 (§3.2) |
| ocean 99% CPU, 无上传、无 `pgl_cs` 加载行 | 桌面 4K, `pgl_g3d_init` 要 2×49.8 MB 连续 shm (fb+z, 1.5x 余量), 池只有 64M → 分配失败 → **静默**退回 CPU 解释器 | `kernel.conf shm_contig_size = 512M` |
| `warp_esr=0x00070010 pc=…` | 错误码 `0x10` = INVALID_ADDR_SPACE, pc 落在唯一的 `STG.E`: UR 操作数里放了缓冲区 VA | 改成 nvcc 约定 (§3.4) |
| ocean 退出后 `g3dtest` 第一个场景 FAIL (全部像素不对, draw 几十 µs 就返回) | `pgl_do_program_free` 只清 `used`: 被 compute 程序用过的槽位留着 `is_compute=1`, 下一个几何程序复用它时误走 compute 路径 | 装载时把槽位整体清零 (已测: ocean → 退出 → g3dtest 7/7 → ocean, 反复两轮) |

**`warp_esr[15:0]` 是错误码枚举, 不是位图** (源码: nvgpu gr 寄存器头文件的 `warp_esr_error_*` 定义): `0xd` oor_reg, `0xe` oor_addr, `0xf` misaligned_addr, `0x10` invalid_addr_space, `0x12` invalid_const_addr_ldc, `0x17` mmu_fault。旧版本文档按位图解读成 "分支跑飞", 是错的。

### 4.3 真机调试方法
- 驱动加载行带 SASS 的 FNV-1a (`fnv=`), 与 host `emu_ocean` 打印的 `fnv=` 对比, 可区分 "板上编出来的不一样" 和 "GPU 侧 fault"。
- fault 时 postmortem 打印 `warp_esr` 和 `pc=`; `pc − 代码基址` (页对齐) 即 `/tmp/nv-host/ocean.emu.lst` 里的指令偏移。
- ocean 的 printf 在 bgrun 下**能**上串口 (`ocean: fps=`、`ocean: check`、`EGL_PROFILE`/`PGL_PROFILE` 行都看得到, 已测; 本文档旧版写 "看不到" 是错的); slog 看不到。
- **画面核对 (`ocean check`)**: 固定 `iGlobalTime=10`, 每 30 帧打一行 `ocean: check WxH hash=… rowJ: rrggbb×16` (整帧 RGB 的 FNV-1a + 16×9 采样网格, 每次报一行)。host 侧 `/tmp/nv-host/emu_ocean 3840 2160 10 samples` 出同格式的参考。
  天空两行 (row7/row8) 板上与 host 逐字节相同; 海面不可比 (噪声 `fract(sin(h)*43758.5)` 把硬件 MUFU 与 libm 的末位差放大成整值差)。同一份编译产物的 hash 应帧间不变, 不改数值语义的优化应保持 hash 不变。
- **板上 shell 不展开通配符**: `rm /tmp/pgl_shader_cache/*` 什么都不删。换编译器变体前必须按文件名逐个删 shader 磁盘缓存 (`1087_*.bin`), 并用加载行的 `fnv=` 确认生效。没有 `env` 命令; `export X=1` 对子进程有效; 不支持 `>>`。
- GPU fault 之后必须复位才能再跑。
- GPU 路径的各种 decline 都是静默的 (退回 CPU 解释器), 判断是否真走 GPU 看有无 `kernel pgl_cs` 加载行 + `ocean fpslog` 的 fps (CPU 4K 下为 0, 不打印)。
- klog 连发会被 UART overrun 丢行, 诊断 klog 要稀疏。

### 4.4 帧时间构成与 present 路径 (已测, 4K)

| 阶段 | 最初 | 现在 | 说明 |
|---|---|---|---|
| GPU launch | 40.1 ms | 31.3 ms (固定帧; 动画中 25–37) | §3.6 |
| swap 里的整帧拷贝 (blit) | 31.1 ms | 5.5 ms | 多线程抢小条 |
| swap 里的 `graph_clear` | ≈6 ms | 0 | 不 letterbox 时不清 |
| 应用每帧 glClear | 2.9 ms | 0 | 全屏 quad 覆盖每个像素 |

- **拷贝为什么慢**: GPU 目标是物理连续 shm, 内核对它一律映射 NOCACHE (`MT_NORMAL_NC`), 单核读约 1 GB/s。xserverd 先于 g2dd 启动, `has_g2d()` 把失败缓存住, 窗口画布是普通 shm → `graph_blt` 的 g2d 路径拒绝 (目的不连续) → 进程内 NEON 拷贝去读 NOCACHE。
  `etc/xwin/init.rd` 里 g2dd+VIC 默认关是另一会话有实测依据的决定 (连续画布让合成器 alpha 混合更慢), 内核的 NOCACHE 策略也是有意的, 都没动。
- **多线程拷贝** (`egl_api.c` `egl_copy_frame`): NOCACHE 读的吞吐随发起读的核数线性涨 (2 带 15.6 ms, 4 带 8.0 ms)。内核只在创建时选核、不迁移, 线程可能撞核 (4 带撞核 15.4 ms, 6 带撞核 10.6 ms), 所以改成所有线程从一个计数器上取 32 行的小条, 耗时只取决于占到了几个不同的核。小条版本板上已测: worker 数 1 / 3 / 5 / 7 → 每帧 15.5 / 10.6 / 5.5 / 5.6 ms; 默认取 "核数 − 1" (本板 5)。swap 整体 37 → 9.5 ms。
- 对比帧率必须同时抓 `ps` (各核 idle), 否则撞核与否会被当成优化效果。

---

## 5. 构建与部署

### 5.1 构建
- GL 栈: `make -C projects/opengl/libs/gles HW=orin.nano PGL_G3D=yes PGL_G3D_FRONTEND=yes PGL_G3D_COMPILER_DIR=<nvcompiler绝对路径>`
  → `libGLESv2.a` (含前端 + 本后端 + portablegl)。
- ocean: `make -C projects/opengl/apps/ocean HW=orin.nano PGL_G3D=yes`。**改编译器后须 `rm aarch64/ocean.orin.nano.o` 强制 relink**
  (Makefile 不跟踪 libGLESv2.a mtime)。ocean `GPU_HWS` 已加 `orin.nano`。
- 驱动: `make -C system/libs/ga10b` (libga10b.a); `make -C system/libs/bsp`; g2dd 经 `make -C system gui` (或 `make x`) relink。
  **默认 `make all` 只 basic+usb, 不含 g2dd; 桌面要 `make x`。**

### 5.2 两棵 build 树 (重要)
- 共享 `system/build_aarch64/orin.nano/` (projects/gles/ocean 输出)。
- machine-local `machines/nvidia/orin.nano/system/build_aarch64/` (**efi `SYSTEM_ROOT` 打包源**, `make x` 输出)。
- projects 输出在共享树, **须 cp 到 machine-local `apps/ocean/`** 再 efi 打包, 否则部署的是旧二进制。
- g2dd 链共享 `libga10b.a`。

### 5.3 cb0 布局契约 (`nv_meta.h`, 编译器↔daemon)
QMD launch state 占头部: `[0x00]blockDim` / `[0x0c]gridDim` / `[0x28]stackPtr`。
`NV_UNIFORM_BASE_OFF=0x40` (uniform blob, 后端 load_uniform 偏置 0x40, 避开 launch state);
`NV_SSBO_BASE_OFF=0x200` (SSBO 基址 VA, block i 在 0x200+i*8); `NV_CB0_SIZE=0x240`。
`[0x118]` 须保持 0 (`.E` 访存的 UR 操作数从这里取, 见 §3.4)。
uniform 须 < 0x1c0 字节 (112 dwords)。daemon `ga10b_kernel_launch_compute` 填这些槽。

### 5.4 部署
- loader 64MB→**128MB** (`efi/main.c` 3 处: lz4_unwrap usize 上限 / net max_size / load_file fallback)。全桌面镜像 ~91MB 超原 64MB 限。
- `make -C efi X=1 initramfs` 打全桌面 (含 ocean + g2dd)。PXE 直接服务 efi 目录。
- 当前启动 profile: init 起完 xwin 桌面后落到串口 rescue shell (桌面进程已在跑, **不要再手动起一遍**, 否则得到两套 xserverd)。确需手动起桌面时:
  `/bin/ipcserv /drivers/xserverd /dev/x; /bin/bgrun /sbin/x/xmouse /dev/mouse0; /bin/bgrun /sbin/x/xim_none /dev/keyb0; /bin/bgrun /bin/x/xsession misa`
- 手动起 g2dd (GPU opt-in): `echo GA10B_ENABLE=1 > /tmp/ga10b.cfg; /bin/ipcserv /drivers/orin.nano/g2dd /dev/g2d`
- rig 仅 serial 无 FB readback, **渲染效果需目视面板**。Pico 掉线需物理重插 USB。详见 [reference-orin-test-rig] memory。

---

## 6. 已排除项 (勿重复尝试)

- **GR 环境坏**: 否。fresh GR 几何 g3dtest 7/7 PASS。
- **控制流分支跑飞**: 否。那是对 `warp_esr` 的错误解读 (§4.2)。
- **板上编译与 host 不一致 / 编译卡死**: 否。FNV 一致, 板上 0.1 s 编完; host 上 ASan+UBSan 全绿。
- **GR bring-up / golden-ctx / firmware / QMD builder / cubin 解析**: 与可用镜像逐文件比对一致, 非差异源。

---

## 7. 遗留项

1. **目视确认**板上 ocean 画面 (host 仿真画面是正确的海面 + 天空; 板上只验证了执行不 fault 和帧率)。
2. **性能**: 4K 18–23 fps (已测, 动画模式; 固定帧 launch 31.3 ms + swap 9.5 ms)。
   之后的方向按收益排: (a) GPU 渲染与 CPU present 重叠 (双缓冲异步 launch, 帧周期从两者之和变成较大者; 要动 GL 库的同步语义, HUD 这类 CPU 后绘制会强制同步); (b) present 改走 2D 引擎 / 直接呈现 (牵涉 X 与 g2dd 启动顺序, 平台决定); (c) 降分辨率渲染 + GPU 侧放大 (现有 `scaleN` 的放大在 CPU 上, 更慢); (d) 按依赖算 delay (§3.6); (e) sin/cos 311 条 MUFU 没有等价的便宜替代 (多项式约 12 条 ALU, 不省; 和角公式会破坏噪声格点一致性)。
3. **compute 模块不回收**: 每次装载 compute 程序 (每启动一次 ocean) 都 `ga10b_dev_module` 一份代码 + 常量 bank, 程序释放时不归还 — libga10b 没有 module unload, 它的 DMA 池实际是 bump arena。g2dd 重启 (即复位) 才回收。反复启动 ocean 很多次后会耗尽, 未测上限。
4. **覆盖面**: 不支持纹理采样、64-bit 值、间接 uniform、非常量 SSBO block; 遇到会编译失败并给出原因 (不会产出错误代码)。
5. **提交状态** (均未 push):
   - `machines` 仓 `main`: nvcompiler 全部源码与 host 测试、libga10b 的 compute 加载/launch 路径、`make.inc`、`kernel.conf`、loader 128MB、本文档。
   - `projects` 仓分支 `orin-ocean-gpu` (基于板测所用的 `2d4046e6`, 落后主仓钉住的 `origin/main` 21 个提交, 上游其间把应用输出目录改到了 `apps/opengl/`; 主仓的 projects 指针未动): ocean `check` 模式与去 glClear、`GPU_HWS` 加 `orin.nano`、EGL present 多线程拷贝。
   - **仍未提交, 属于其他会话**: libga10b 的不透明 draw 跳排序 (`ga10b_pgl.c/.h` 的部分 hunk、`pgl_geom.cu` 与 cubin、`test_pgl_order.c`、`ga10b-3d-handover.md`); `projects` 里 portablegl 的 profile 计时 / cull sign / uncached 读回修复、teapot/3ddemo、`gles/Makefile` 的 `PGL_G3D_FRONTEND=no` 分支、`GPU_HWS` 里的 `ego`。板测镜像是带着这些改动编出来的。
   - **`projects/cuda` 整个目录未入库** (另一条工作线的 libcuda): libga10b 的 Makefile 和 `tests/host/Makefile` 都引用它的 `cubin.c` / QMD builder, 干净检出时编不过, 需要它的主人提交。

---

## 8. 接手清单

1. host: `make -f tests/host/Makefile.host emu EMU_ARGS="160 120"`, 应输出 `DIFFERENTIAL PASS` 和 `3738 instrs … fnv=19c39285`。
2. 改后端后: 先过 host 差分 + `check_disasm.py`, 再上板。
3. 真机: 构建 (§5.1) → `make x` → `make -C efi X=1 initramfs` → 复位 → 登录 → 起 g2dd (§5.4) → `g3dtest` → `/bin/bgrun /apps/ocean/ocean fpslog`。
4. 期望串口: `ga10b: kernel pgl_cs text=67088 fnv=dfe6e57d regs=57 …`, 随后每秒一行 `ocean: fps=N`, 无 `warp_esr`。
