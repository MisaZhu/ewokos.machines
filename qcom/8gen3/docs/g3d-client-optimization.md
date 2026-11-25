# 8gen3 portablegl GPU 路径：客户端侧优化方向

2026-11-24。来源是 Orin Nano 上的一轮帧时间定位（记录在
`machines/nvidia/orin.nano/docs/ga10b-3d-handover.md` §9.7、§9.8）。
其中有几项落在各平台共用的代码里，8gen3 同样受影响。

**本文所有耗时数字都是在 Orin 上测的，8gen3 上一项都还没测。**
标注：**已测(Orin)** = Orin 真板实测；**源码** = 读 8gen3 / 共用代码核对；
**未测** = 8gen3 上待验证。

## 1. 主项：像素窗口包围盒从 uncached 共享内存读回顶点

### 现象（已测，Orin）

`projects/opengl/libs/gles/portablegl/src/pgl_g3d.c` 的
`draw_mesh_compiled_slot()` 在提交每个 draw 之前，为光栅核的
`uDispatchOffset` 像素窗口计算投影包围盒（当前 1669 行起的
`if (!geom_texcol && !geom_inst)` 分支）。循环对每个顶点读
`_pos[v * stride + 0..2]`，再乘 `uMVP`。

`_pos` 是刚写完的 staging 画布，属于物理连续共享内存。内核把这类段对所有
平台都映射成 NOCACHE（`kernel/kernel/src/mm/shm.c` 的 `shm_proc_map`：
`it->contig ? PTE_ATTR_NOCACHE : PTE_ATTR_WRBACK`），所以这里每个标量读都是
一次不经缓存的内存访问。

Orin 上 teapot（每帧 19200 顶点，约 5.8 万次读）：

| 每帧 ms | 跑这段循环 | 不跑 |
|---|---|---|
| stage（`pgl_g3d` 内 IPC 之前的全部时间） | 5.78 | 0.36 |
| 帧时间 / 帧率 | 12.7 / 78.6 fps | 6.6 / 150.7 fps |

折合每顶点约 0.28 µs，与顶点数成正比。

### 8gen3 的情况（源码）

- Orin 的核没有 `uDispatchOffset`，算出来的窗口没有去处，所以 Orin 上的
  修法是"没有这个 uniform 就不跑循环"。
- 8gen3 **不能这样修**：Adreno 的像素核声明了 `uniform ivec2 uDispatchOffset`
  （`system/libs/adrenocompiler/adreno_geom_source.h`，`Q8B_GEOM_PIXEL_BODY`，
  颜色和纹理两个变体都有），窗口用来把光栅网格收缩到图元包围盒，是有效
  功能。循环必须保留，问题只在它读的是哪块内存。
- `PGL_G3D=yes`、staging 画布同样来自 `g2d_shm_alloc_phy`，所以这段循环在
  8gen3 上每个网格 draw 都在跑。

### 方向

让包围盒读**缓存里的源数据**，不读 `_pos`。同一个函数里上面的 staging
分支已经把源指针分清楚了：

- 有 `m->packed`：位置是 `m->packed + (first + v) * m->packed_stride` 的前
  3 个 float；
- 否则：`m->pos + (first + v) * 3`。

`_pos` 里的位置就是从这两处原样拷过去的（`memcpy` 或逐 float 赋值），数值
逐位相同，所以包围盒结果不变。更省的做法是把 min/max 累加并进 staging 的
拷贝循环，源数据只读一遍；但那要改四个分支，先做上面的最小改动即可。

`geom_texcol`、`geom_inst` 两条路线本来就不进这个分支，不受影响。

### 验证方法（未测）

1. 用 `PGL_PROFILE=1` 跑 teapot，看每 5 秒一行的
   `PGL_PROFILE cpu ... stage=`（窗口累计，除以 `frames=` 得每帧）。改前
   改后各取一次，stage 应按顶点数下降。这行计时是这一轮加进
   `pgl_engine.c` / `pgl_g3d.c` 的，写本文时还没提交。
2. 正确性：窗口是保守包围盒，算错的表现是图元被裁掉一块。挑图元只占
   屏幕一小块的场景（窗口收缩最明显；代码注释里举的例子是 fireworks 的
   小爆点）和 teapot，对比改前改后的画面。
3. 关注 `first != 0` 的子区间 draw：源指针要带 `first`，`_pos` 是重定基到
   0 的，这是最容易写错的地方。

## 2. 其余共用代码里的项

这些也是在 Orin 上看到的，按代码推断 8gen3 同样存在，量级需要在 8gen3 上
重测后再决定做不做。

| 项 | 位置 | Orin 上的数字 | 8gen3 |
|---|---|---|---|
| demo 主循环每帧 `usleep(1000)` | `projects/cube/cube.cpp`、`opengl/apps/teapot`、`opengl/apps/3ddemo` 的主循环回调 | 按 1024 Hz tick 推算每帧睡 1–2 ms（未直接测） | `kernel.conf` 同为 `timer_freq = 1024`，推算相同（未测） |
| 进程只在创建时选核，不迁移 | `kernel/kernel/src/proc.c` 的 `core_fetch()` 按任务数选核；IPC worker 固定在客户端所在核 | 三个 GL 应用同开时出现过 3 个核只剩约 30% 空闲、另 3 个核 96–99% 空闲（已测） | 8 核，同一份内核代码；是否出现取决于启动时的落核（未测） |
| swap 时把后备缓冲拷进窗口画布 | `opengl/libs/gles/src/egl/egl_api.c` 的 `egl_on_repaint()` | 每帧约 1.2 ms，CPU 读 uncached 的 640x480 帧（已测） | 源图带了 `shm_id`，`/dev/g2d` 在位时 `graph_blt` 会走 g2d 拷贝，可能已经是 GPU 在拷。看 `EGL_PROFILE` 的 `blit=`（未测） |
| 引擎逐顶点整理 | `pgl_engine.c` 的 `gpu_tessellate*` | teapot 每帧约 0.9 ms（已测） | 同一份代码（未测） |

## 3. 不适用于 8gen3 的部分

Orin 这一轮在 GPU 核上的改动（不透明 draw 跳过逐 tile 排序、放开 tile 链表
容量）针对的是 `machines/nvidia/orin.nano/system/libs/ga10b` 里的 CUDA 核。
8gen3 用的是 Adreno 自己的像素核和 64 像素粗分箱，结构不同，没有评估过
是否有对应问题。
