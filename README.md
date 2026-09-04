# ewokos.machines

Board support and per-target ports of EwokOS. Every directory under `machines/`
is a self-contained bring-up recipe that pairs a kernel BSP with a rootfs build,
so a port can be compiled, imaged, and deployed without touching the shared
`system/` and `kernel/` trees.

The most active hardware work lives here: the two Raspberry Pi families
(`raspi5/`, `raspix/`) carry the VideoCore GPU 2D engine, the RP1/xHCI/DSI
device stacks, WLAN, camera, audio, and a large add-on overlay ecosystem. The
remaining directories are QEMU and embedded-board ports (Rockchip, Allwinner,
LEGO EV3, RISC-V, x86) at various stages of completeness.

> The recommended starting point for a pure-QEMU development flow is
> `machine.virt/` at the repository root, not a target under `machines/`. See the
> top-level `README.md` for that quick start.

## Directory Layout

```text
machines/
├── README.md               # this file
├── LICENSE                 # Apache-2.0
├── clockwork/
│   ├── picocalc/           # ClockworkPi PicoCalc (Rockchip RK3506)
│   └── uconsole/           # ClockworkPi uConsole overlay (rides on raspix / CM4)
├── lego.ev3/               # LEGO Mindstorms EV3 (ARM926EJ-S)
├── miyoo/                  # Miyoo retro handheld (Allwinner sunxi)
├── orangepi/               # Orange Pi (Allwinner)
├── raspi5/                 # Raspberry Pi 5 (BCM2712, VideoCore VII)
├── raspix/                 # Raspberry Pi 1..4 / CM4 (BCM283x, VideoCore IV/VI)
├── virt.riscv/             # QEMU RISC-V virt (rv64)
├── x2lite.rk3128/          # Rockchip RK3128 board
└── x86/                    # x86 / x86_64 PC-style target
```

Board notes (firmware `config.txt` recipes, panel/overlay revisions, SD layout)
live **per target** under `<target>/docs/` — e.g. `raspix/docs/`,
`clockwork/uconsole/docs/`, `orangepi/docs/` — not at the `machines/` root.

## Anatomy of a Machine Port

Most ports follow the same two-directory shape:

```text
machines/<target>/
├── kernel/                 # board support package + boot glue
│   ├── config.mk           # ARCH, ARCH_VER, SMP, PAGE_SIZE, LOAD_ADDRESS, QEMU_MACHINE
│   ├── Makefile            # build kernel image, run/debug in QEMU (where supported)
│   ├── bsp/                # SoC init, MMU/IRQ/timer, mem-map whitelist, boot entry
│   └── *.lds.S / *.dts     # linker script / device tree (per platform)
├── system/                 # rootfs build
│   ├── make.inc            # HW, ARCH, ARCH_VER, BSP_LFLAGS (link contract)
│   ├── Makefile            # basic / network / gui / x / sd targets
│   ├── libs/               # arch_<soc> (+ videocore on Pi) + bsp static libs
│   ├── drivers/            # machine-specific user-space daemons
│   ├── etc/                # init.rd profiles (basic/gui/network/xwin), display.json ...
│   └── apps/               # board-specific applications
└── 3rd/                    # board overlays & third-party device integrations (Pi only)
```

Key build variables (see `kernel/config.mk` and `system/make.inc`):

| Variable | Meaning |
|----------|---------|
| `ARCH` | `arm`, `aarch64`, `rv64`/`riscv`, `x86` |
| `ARCH_VER` | `v5`, `v7`, `v8`, `x64` — selects the arch code path |
| `HW` | board name; artifacts land in `system/build_<ARCH>/<HW>/` |
| `SMP` | `yes`/`no` — multiprocessor support |
| `PAGE_SIZE` | `4k` / `16k` / `64k` (kernel MMU page size) |
| `LOAD_ADDRESS` | physical entry the firmware/bootloader jumps to |
| `BSP_LFLAGS` | link order for the BSP stack, e.g. `-lbsp -lvideocore -larch_bcm2712` |

The rootfs is assembled in layers — `basic` → `network` → `gui` → `x` — and the
`sd` target packs `system/build_<ARCH>/<HW>/rootfs/` into an ext2/ext3 image.

## Platform Support Matrix

Support reflects the checked-in source and build recipes in this tree; hardware
validation depth varies by board.

| Target | Board / SoC | ARCH | GPU / 2D accel | SMP | Run environment | Status |
|--------|-------------|------|----------------|-----|-----------------|--------|
| `raspi5/` | Raspberry Pi 5 · BCM2712 (Cortex-A76) + RP1 | `aarch64` v8 | **VideoCore VII** (V3D 7.1, 12 QPUs) | yes | Real HW (no upstream QEMU model) | Active, most complete HW stack |
| `raspix/` | Raspberry Pi 1..4 / CM4 · BCM283x | `arm` v7 + `aarch64` v8 | **VideoCore IV** (V3D 2.1) + **VideoCore VI** (V3D 4.2) | yes | QEMU (`raspi2b`/`raspi3b`) + real HW | Strong, broad add-on ecosystem |
| `clockwork/uconsole/` | ClockworkPi uConsole · Raspberry Pi CM4 (BCM2711) | `aarch64` v8 | VideoCore VI (inherits `raspix`) | yes | Real HW | Overlay on `raspix` |
| `clockwork/picocalc/` | ClockworkPi PicoCalc · Rockchip RK3506 | `arm` v7 | none (framebuffer) | yes | QEMU (`raspi2b`) + real HW | Specialized board port |
| `x2lite.rk3128/` | Rockchip RK3128 board | `arm` v7 | none (framebuffer) | yes | QEMU (`raspi2b`) + real HW | In-tree port |
| `miyoo/` | Miyoo handheld · Allwinner sunxi | `arm` v7 (Cortex-A7) | none (framebuffer) | yes | Real HW | Active handheld port |
| `orangepi/` | Orange Pi · Allwinner | `arm` v7 | none (framebuffer) | no | QEMU (`raspi2b`) + real HW | In-tree port |
| `lego.ev3/` | LEGO Mindstorms EV3 · ARM926EJ-S | `arm` v5 | none (framebuffer) | no | QEMU (`versatilepb`) + real HW | In-tree port |
| `virt.riscv/` | QEMU RISC-V `virt` | `rv64` | none | no | QEMU | Kernel + desktop bring-up |
| `x86/` | x86 / x86_64 PC | `x86` (`x64`) | none (VBE/framebuffer) | yes (4 cores) | QEMU (`pc`) | Machine-local kernel + system |

Only the two Raspberry Pi families use VideoCore; the Rockchip, Allwinner, EV3,
RISC-V, and x86 ports drive a plain framebuffer with the software/NEON/SSE
`bsp_g2d` paths.

---

## Per-Platform Notes

### `raspi5/` — Raspberry Pi 5 (BCM2712)

- **Config**: `ARCH=aarch64`, `ARCH_VER=v8`, `SMP=yes`, `PAGE_SIZE=16k`,
  `LOAD_ADDRESS=0x80000` (firmware loads `kernel8.img`). Upstream QEMU has no
  `raspi5` machine model, so this port targets **real hardware**.
- **SoC**: BCM2712 (quad Cortex-A76) with the **RP1** south-bridge reached over
  PCIe (Gen2, inbound window offset `0x1000000000`) — USB, GPIO, and the DSI/DPI
  display pipeline all hang off RP1.
- **libs**: `arch_bcm2712` + `videocore` + `bsp` (link order matters —
  `BSP_LFLAGS = -lbsp -lvideocore -larch_bcm2712`).
  - `arch_bcm2712`: VPU property **mailbox**, `xhci`/DWC3 USB 3, `rp1` +
    `rp1_dsi` + `rp1_dpi` (display), `mmc`/`sdhci` (SD), `nvme`, `framebuffer`,
    `native_hdmi` (HDMI/CVT-RB timing), `gpio`, `i2c`, `spi`, `pl011_uart`.
  - `videocore`: **VideoCore VII** V3D 7.1 2D engine (see the VideoCore section).
- **drivers**: `cpud` (temp/governor), `fand` (fan), `uartd`, `wlan` (Broadcom
  SDIO), `fbdisplayd` / `dsi_fbdisplayd`, `nvmefsd` (NVMe root fs).
- **3rd/**: `waveshare.pi` LCD HATs, `sunfounder`, `xpt2046d` resistive touch,
  `4inch-HDMI-Display-C`, `shchv`.
- **Deploy**: user-space binaries are copied from
  `system/build_aarch64/rootfs/drivers/raspi5/` onto the board; `make sd` packs
  a 512 MB ext2/ext3 image (watch the image size if large project assets are
  staged into the rootfs).

### `raspix/` — Raspberry Pi family (BCM283x: Pi 1/2/3/4, CM4)

- **Config**: dual-architecture in one tree —
  `arm` v7 (`LOAD_ADDRESS=0x8000`, QEMU `raspi2b`) and
  `aarch64` v8 (`LOAD_ADDRESS=0x80000`, QEMU `raspi3b`); `SMP=yes`,
  `PAGE_SIZE=4k` (Pi 1..4 do not support 16k pages).
- **libs**: `arch_bcm283x` + `videocore` + `bsp`
  (`BSP_LFLAGS = -lbsp -lvideocore -larch_bcm283x`).
  - `arch_bcm283x`: VPU property **mailbox**, `dwc2` (USB), `sdhost` (SD),
    `i2c_bsc`, `pl011_uart`, `dsi1` (DSI panel), `framebuffer`, `mbox_actled`.
  - `videocore`: **dual GPU path** — VideoCore IV (V3D 2.1, Pi 3) via the SRQ
    launcher and VideoCore VI (V3D 4.2, Pi 4/CM4) via CSD; the SoC is detected at
    init and the matching kernels are dispatched.
- **drivers**: `cpud`, `uartd`, `soundd`, `camd` (Unicam camera), `btd`
  (Bluetooth), `wlan`, `g2dd`, `hid_joystickd`, `fbdisplayd`, `dsi_fbdisplayd`.
- **3rd/**: the largest overlay set — `waveshare.pi` (many LCD/touch HATs),
  `banli`, `gnpe`, `xgo.pi` (robot dog), `unified`, `others`.
- **docs/**: `board revisions`, `raspi_config.txt` (firmware `config.txt`
  recipes per panel/overlay).

### `clockwork/uconsole/` — ClockworkPi uConsole (CM4)

- Not a standalone kernel: it is a **system overlay on `raspix`** (`HW=raspix`,
  `ARCH=aarch64`, `BSP_LFLAGS = -lbsp -larch_bcm283x`), so it inherits the
  BCM2711 VideoCore VI path.
- Adds uConsole-specific **drivers**: `soundpwmd` (PWM audio), `dsi`,
  `fbdisplayd` / `fbdisplay6d` (DSI panel), `powerd` (power/battery).
- `etc/` ships the firmware `config.txt` overlays: `clockworkpi-uconsole` (Pi 4)
  and `clockworkpi-uconsole-cm5` (Pi 5), with `vc4-kms-v3d` and PCIe/UART params.

### `clockwork/picocalc/` — ClockworkPi PicoCalc (Rockchip RK3506)

- **Config**: `ARCH=arm` v7, `BSP=rk3506`, `SMP=yes`, QEMU `raspi2b` for
  bring-up.
- **libs**: `arch_rk3506` (DesignWare `dwmmc`, `gpio`, `i2c`, `spi`,
  `framebuffer`, Rockchip `pinctrl`) + `bsp`. No VideoCore.

### `x2lite.rk3128/` — Rockchip RK3128

- **Config**: `ARCH=arm` v7, `BSP=rk3128`, `SMP=yes`, QEMU `raspi2b`.
- **libs**: `arch_rk3128` (`dwmmc`, `framebuffer`, `sd`) + `bsp`. No VideoCore.

### `miyoo/` — Miyoo handheld (Allwinner sunxi)

- **Config**: `ARCH=arm` v7 (`CPU=cortex-a7`), `SMP=yes`.
- **libs**: `arch_miyoo` (`framebuffer`, sunxi `sdmmc`/`sd`) + `bsp`. No
  VideoCore; handheld-specific audio/graphics/storage setup.

### `orangepi/` — Orange Pi (Allwinner)

- **Config**: `ARCH=arm` v7, `SMP=no`, QEMU `raspi2b`.
- **libs**: `arch_orangepi` (minimal `sd`) + `bsp`. Kernel/system/board
  packaging are present; the port is intentionally small.

### `lego.ev3/` — LEGO Mindstorms EV3

- **Config**: `ARCH=arm`, `CPU=arm926ej-s`, `ARCH_VER=v5` (ARMv5), `BSP=ev3`,
  QEMU `versatilepb`.
- **libs**: `arch_ev3` (`gpio`, `sdmmc`, `spi`, `uart`, `framebuffer`) + `bsp`.
  No SMP, no VideoCore; includes EV3-specific drivers and packaging.

### `virt.riscv/` — QEMU RISC-V virt

- **Config**: `ARCH=rv64`, `-march=rv64g_zifencei`, `SMP=no`, `DEBUG=yes`; QEMU
  `virt` loads `root.ext2` at `0xe0000000`.
- **libs**: `arch_virt` (`sd`) + `bsp`. Base, GUI, and X flow are present;
  network integration is lighter than `machine.virt`.

### `x86/` — x86 / x86_64 PC

- **Config**: `ARCH=x86`, `ARCH_VER=x64`, QEMU `pc`, `SMP=yes`
  (`QEMU_SMP_CORES ?= 4`), `LOAD_ADDRESS=0x00100000`.
- **libs**: `bsp` only — the x86 arch code lives in the shared kernel platform
  layer. Full machine-local kernel and system recipes with the GUI/X stack.

---

## VideoCore (Raspberry Pi GPU + VPU firmware)

This is the deepest and most Pi-specific part of the tree, and the word
"VideoCore" covers **two distinct things** that both live on the Pi ports:

1. The **VideoCore VPU firmware mailbox** — the property/register interface the
   ARM core uses to talk to the SoC firmware (allocate a framebuffer, query the
   display, set clocks and power domains). Implemented in `libarch_bcm2712`
   (raspi5) and `libarch_bcm283x` (raspix).
2. The **VideoCore GPU (V3D QPU array)** — programmed directly as a compute
   engine to accelerate offline ARGB8888 2D drawing (fill, blit, alpha blend,
   rotate, scale, copy). Implemented in `libvideocore`.

### 1. VPU Firmware Mailbox (`libarch_bcm2712` / `libarch_bcm283x`)

The ARM side and the VideoCore VPU exchange messages through a hardware mailbox
(at peripheral offset `0x13880` on BCM2712; the standard mailbox window on
BCM283x). Two channels are used:

| Channel | Value | Purpose |
|---------|-------|---------|
| `PROPERTY_CHANNEL` | 8 | Structured property tags (get/set SoC & display state) |
| `FRAMEBUFFER_CHANNEL` | 1 | Legacy framebuffer allocation |

Buffer addresses are OR'd with a **VC bus alias** so the firmware sees the right
memory attribute:

| Alias | Value | Meaning |
|-------|-------|---------|
| `MAILBOX_VC_ALIAS_NONCACHED` | `0x40000000` | uncached access |
| `MAILBOX_VC_ALIAS_COHERENT` | `0xC0000000` | cache-coherent access |

Property tags in use include framebuffer/display control (`ALLOCATE_BUFFER`
`0x40001`, get/set physical & virtual width/height, depth, pixel order, pitch,
virtual offset, `GET_EDID_BLOCK`, `GET_NUM_DISPLAYS`) and clock control
(`GET_CLOCK_RATE` `0x30002`, `GET_MAX_CLOCK_RATE` `0x30004`, `SET_CLOCK_RATE`
`0x38002`). Consumers:

- **`framebuffer.c`** allocates the scan-out buffer and queries the panel/HDMI
  geometry (structured tag blocks mirroring Circle's `bcmpropertytags.h`).
- **`v3d_g2d.c`** pins the **V3D clock** (`FW_CLOCK_V3D = 5`) to its maximum at
  init and reads the actual rate back, reported through
  `bsp_g2d_clock_hz()` / the `g2d` `GET_CLOCK` device control.

`bcm2712_mailbox_call_timeout()` is the non-blocking entry point: it returns on
timeout instead of spinning forever, which keeps a firmware that never answers
from hanging the calling daemon.

### 2. VideoCore GPU 2D Engine (`libvideocore`)

Instead of going through the firmware's rendering stack, EwokOS programs the
**V3D QPU array directly** as a general-purpose compute device (CSD / SRQ
launch) and runs hand-written QPU kernels that operate on the caller's ARGB8888
buffers **in place** (zero copy). Three VideoCore generations are supported:

| SoC | Machine | VideoCore | V3D | QPUs | Launch path | Bring-up specifics |
|-----|---------|-----------|-----|------|-------------|--------------------|
| BCM2712 | `raspi5` (Pi 5) | **VII** | 7.1 | 12 (fixed) | CSD | `hub "VHUB"` + `core0` + SMS power-up; registers mapped via `SYS_MEM_MAP` (V3D sits outside the main MMIO window and is whitelisted by the kernel `check_mem_map_arch`) |
| BCM2711 | `raspix` (Pi 4 / CM4) | **VI** | 4.2 | probed | CSD | identity-mapped peripheral window; V3D sits behind a stopped AXI async bridge that only the PM/ASB register sequence opens |
| BCM2837 | `raspix` (Pi 3) | **IV** | 2.1 | ≤ 16 (1 thread/QPU) | SRQ | no usable CSD — kernels run through the SRQ user-program launcher (GPU_FFT protocol); powered via the firmware GRAFX domain call |

The SoC is identified at init (`v3d_g2d_ver()` returns `21` / `42` / …, QPU
count probed via `g2d_count_qpus()`), and the matching kernel set + submission
path is selected per dispatch.

#### g2d three-layer architecture

VideoCore code is isolated in its own static library, `libvideocore.a`, split
into three layers (the same shape on both raspi5 and raspix):

```text
graph lib / xserverd compositor
        │  g2dclient (IPC)
        ▼
g2dd daemon           system/gui/drivers/g2dd/g2dd.c   (stateless /dev/g2d char dev,
        │                                              shm/dma canvas attach + cache)
        │  bsp_g2d_*()
        ▼
libbsp  bsp_g2d.c     machines/<hw>/system/libs/bsp/src/g2d/bsp_g2d.c
        │             public bsp_g2d API, arg validation, rect clipping,
        │             scalar CPU + arch NEON/SSE paths, #include <videocore/vc_g2d.h>
        │  gpu_*_op() / gpu_*_surface()
        ▼
libvideocore vc_g2d.c Q15 affine-map math, GPU eligibility gate,
        │             large-surface banding/tiling policy, QPU dispatch
        │  v3d_g2d_*()
        ▼
libvideocore v3d_g2d.c hardware bring-up (map regs, probe IDENT, power/clock,
        │              SMS/ASB, L2 cache), cache coherence, CSD/SRQ submission
        │  mailbox property calls + MMIO register writes
        ▼
VideoCore VPU firmware + V3D QPU array (the GPU)
```

- **Public header**: only `include/videocore/vc_g2d.h` is exported; `v3d_g2d.h`
  and the generated kernel headers stay private to the library.
- **`g2d_map_t`** is the Q15 fixed-point affine coefficient set shared verbatim
  with the kernel uniform layout:
  `u = (pu*X + qu*Y)>>15 + cu`, `v = (pv*X + qv*Y)>>15 + cv`. `g2d_map_params()`
  builds crop→rect maps (with 0/90/180/270 rotation), `g2d_map_rotate()` builds
  arbitrary-angle whole-surface rotation maps, and `g2d_rotated_size()` returns
  the bounding box for any angle.

#### QPU kernels

All kernels are ARGB8888, run on every QPU at once (lane 0..15 = a 16-pixel
group, `qid` selects the row band), and redirect out-of-rect lanes to a scratch
sink so any width/height is safe.

- **raspi5 (V3D 7.1, CSD)** — 8 kernels, pre-generated in
  `g2d_qpu_kernels.h`:
  `argb_fill`, `argb_fill4` (vec4), `argb_blit`, `argb_alpha`, `argb_rotate`,
  `argb_rot90` (dedicated right-angle, contiguous source reads),
  `argb_scale_pow2` (aligned power-of-two downscale), `argb_copy` (1:1 vec4).
- **raspix** — two generated sets:
  - **V3D 4.2 (CSD)** for Pi 4: `argb_fill`, `argb_blit`, `argb_rotate`,
    `argb_alpha`, `argb_rot90` (in `g2d_qpu_kernels*.h`).
  - **VC4 / V3D 2.1 (SRQ)** for Pi 3: `argb_*_vc4` plus `_loop` / `_gather`
    variants — VC4 has no TMU store, so every group is written through VPM and
    DMA'd out by the VDW (GPU_FFT store protocol).

The raspix tree also ships the **QPU assembler toolchain** under
`videocore/src/tools/` (`qpuasm.py` — a VC4/V3D 4.2 assembler/disassembler,
`gen_kernels.py`, `gen_vc4_loop.py`) so the instruction streams can be
regenerated in place; the raspi5 kernels are checked in pre-generated.

#### Execution mechanics worth knowing

- **Zero copy, no V3D MMU**: kernels read/write the caller's buffers through
  their **physical** addresses. Because the QPU has no MMU, every caller-supplied
  `*_phy` must pass a RAM-range validation gate (`v3d_g2d_phy_valid`) covering the
  allocable range, the `sys_dma` window, and the `IPC_CONTIG` shm slab.
- **Eligibility gate** (`gpu_phys`, `gpu_ok`, `gpu_map_fits`, `gpu_clip_rect`):
  the buffer must be physically contiguous (contig shm slab or `sys_dma`), carry a
  resolved physical base, and fit the 32-bit TMU address space (< 4 GB). Map
  coefficients must fit the kernel's signed 24×24 multiplies.
- **No CPU fallback inside `libvideocore`**: an ineligible or failed dispatch just
  reports failure; `bsp_g2d.c` decides whether to use its scalar/NEON CPU paths.
  A **timed-out dispatch is never replayed** (it may still own or have partially
  written the destination). On raspix this is a three-state result
  (`GPU_DONE` / `GPU_UNSUPPORTED` / `GPU_FAILED`) so "not my job" is told apart
  from "attempted and failed"; raspi5 returns non-zero for success.
- **Preloaded kernels**: instruction streams are staged once at init into
  physically-contiguous `dma_alloc` memory; a dispatch refreshes only the small
  uniform block.
- **Cache coherence**: a standalone dispatch brackets itself with PRE (drop stale
  V3D L2/slice lines, clean ARM sources) and POST (drain the TMU write combiner,
  clean L2 to DRAM, invalidate the ARM destination). `NOCACHE` dma canvases skip
  it. Banded/tiled large-surface ops share one bracket (first band PRE, last band
  POST) with a per-band uniform-visibility barrier in between.
- **Large-surface batching (raspi5)**: destinations past `G2D_BIG_SURFACE`
  (4 MB — the measured L2 walk-order cliff) are split into 512 KB horizontal
  **bands** (blit/copy/alpha/scale) or 256×256 **tiles** (rotate, whose walk
  crosses the source diagonally), keeping each dispatch cache-sized.
- **vec4 fast path (raspi5)**: `argb_copy` / `argb_fill4` move 16 bytes per lane
  per TMU request, gated by a one-shot hardware probe (`v3d_g2d_vec4_ok`) that
  parks the fast kernels if the silicon rejects them.

#### Building & linking

- `libs/Makefile` enforces the order `arch_bcm2712|arch_bcm283x → videocore →
  bsp`: `videocore` and `bsp` include headers installed into the SDK include dir
  by the arch lib, and `bsp_g2d` additionally includes `videocore/vc_g2d.h`.
- `libvideocore.a` is installed into `system/build_<ARCH>/<HW>/lib` and
  `vc_g2d.h` into the SDK include dir.
- Final link uses `BSP_LFLAGS` from `make.inc`; the order
  `-lbsp -lvideocore -larch_bcm<soc>` matters because `bsp_g2d` calls into
  `videocore`, which in turn calls the `arch_bcm<soc>` mailbox helpers.

#### raspi5 vs raspix at a glance

| Aspect | `raspi5` (BCM2712 / VC7) | `raspix` (BCM283x / VC4+VC6) |
|--------|--------------------------|------------------------------|
| V3D versions | 7.1 only | 2.1 (Pi 3) **and** 4.2 (Pi 4), chosen at init |
| Register access | `SYS_MEM_MAP` window (outside main MMIO) | identity-mapped peripheral window |
| Power/launch | SMS power-up + hub AXI kick, CSD | AXI async bridge (PM/ASB) for Pi 4 CSD; firmware GRAFX + SRQ for Pi 3 |
| Dispatch API | `gpu_*_op()` wrappers own banding/tiling + kernel choice; `gpu_*_surface()` = one rect | version dispatch inside `gpu_fill` / `gpu_rot90_surface` via `v3d_g2d_ver()` |
| Result model | non-zero = success | three-state `GPU_DONE` / `GPU_UNSUPPORTED` / `GPU_FAILED` |
| QPU assembler | kernels pre-generated | `src/tools/` (qpuasm.py + generators) in-tree |

---

## Building a Machine Port

The common flow (QEMU-capable targets can also `make run` / `make debug` from
the kernel dir):

```bash
# 1. build the root filesystem (stages into system/build_<ARCH>/<HW>/rootfs)
cd machines/<target>/system
make x          # basic -> network -> gui -> x  (or: make basic / network / gui)

# 2. pack the SD/eMMC image (ext3 by default; FS=ext2 supported)
make sd

# 3. build the kernel image
cd ../kernel
make            # QEMU targets: make run / make debug / make gdb
```

Real-hardware Pi targets (`raspi5`, and `raspix` on silicon) skip QEMU: copy the
kernel image and the staged rootfs (or the `sd` image) to the boot partition /
SD card, then apply the matching firmware `config.txt` overlays (see
`raspix/docs/raspi_config.txt` and the `3rd/` overlay trees).

## Notes

- Board-specific Makefiles under `<target>/system/drivers/` are maintained by the
  port owner; when a driver directory lacks a Makefile it is simply not wired
  into the build.
- If a directory referenced here is missing locally, the `machines/` tree may be
  a submodule in your checkout — initialize submodules before building.
- The `3rd/` overlay trees (Pi only) integrate Waveshare/SunFounder LCD+touch
  HATs and other add-ons; each carries its own display config and driver glue.
