/*
 * v3d_g2d.h - VideoCore (V3D) hardware back end for the EwokOS raspix
 * bsp_g2d layer (Raspberry Pi 3 / BCM2837 and Pi 4 / BCM2711).
 *
 * Register block layout follows the Linux drm/v3d driver (v3d_regs.h):
 *
 *   V3D 4.2 (BCM2711, Pi4/CM4): hub at mmio_base+0xC00000 (HUB_IDENT0
 *     reads "VHUB"), core0 at +0x8000 with the CSD compute engine at
 *     0x900..0x94x (ver < 71 offsets), INT_CSDDONE = bit 7.
 *   V3D 2.1 (BCM2837, Pi3): single-core block, no hub, no usable CSD -
 *     the VC4 kernels run through the SRQ user-program launcher
 *     instead (the GPU_FFT protocol: one thread per QPU, firmware
 *     tag 0x30012 enables QPU access first).
 *
 * Unlike raspi5 the whole V3D + PM window already sits inside the
 * shared 32 MB MMIO window (_mmio_base), so no SYS_MEM_MAP call is
 * needed.  CSD code/uniform staging lives in physically-contiguous
 * sys_dma buffers (dma_alloc), so the QPU fetches them by physical
 * address without a V3D MMU page table.
 *
 * Canvases arrive as virtual addresses (shmat() / dma window addresses)
 * with their resolved physical base supplied by the caller (the bsp_g2d
 * *_phy parameters); v3d_g2d_run() maintains the ARM/V3D caches around
 * the dispatch and the kernels write the physical addresses directly.
 *
 * ZERO COPY: the GPU never copies canvas pixels.  The kernels write and
 * read the caller's buffers in place through their physical addresses;
 * the only memory the driver touches is the (small) uniform block, which
 * is refreshed per call.  The kernels are preloaded once at init, so a
 * dispatch copies nothing but the uniforms.
 */

#ifndef V3D_G2D_H
#define V3D_G2D_H

#include <stdint.h>
#include <stddef.h>
#include <ewoksys/ewokdef.h>

/* Probe registers, bring up power/clock and allocate dma staging;
 * idempotent.  Returns 0 when the GPU is usable (hardware present AND
 * at least one QPU kernel loaded) and a real failure otherwise. */
int v3d_g2d_init(void);

/* Non-zero once the GPU is usable. */
int v3d_g2d_ready(void);

/* V3D architecture version x10 (42 = V3D 4.2 Pi4, 21 = V3D 2.1 Pi3). */
int v3d_g2d_ver(void);

/* Number of QPUs available for CSD dispatch (read from IDENT1:
 * (QUPS+1) * (NSLC+1); 12 on BCM2711). */
int v3d_g2d_num_qpus(void);

/* Physical address of the TMU write-scratch surface the kernels use for
 * out-of-rect writes and the flush epilogue (feed it to uniforms). */
uint32_t v3d_g2d_scratch_phys(void);

/* Address validation gate: returns non-zero when [phy, phy+bytes) lies
 * inside a legitimate physical RAM region (the allocable memory range,
 * the sys_dma window, the IPC_CONTIG shm slab, or below the total
 * physical memory size).  The GPU kernels write through 32-bit physical
 * addresses with no MMU, so a bad address would corrupt arbitrary
 * memory; every caller-supplied *_phy must pass this check before a
 * dispatch is allowed. */
int v3d_g2d_phy_valid(ewokos_addr_t phy, size_t bytes);

/* One-shot hardware probe of the vec4 (TMUC general-access) TMU path
 * used by the argb_copy/argb_fill4 kernels: runs a small GPU copy into
 * scratch and verifies it on the CPU.  Non-zero when the fast vec4
 * kernels are usable; callers must fall back to the single-word kernels
 * otherwise.  The result is cached after the first call.  Always 0 on
 * VC4 (V3D 2.1) where the CSD kernels cannot run. */
int v3d_g2d_vec4_ok(void);

/* Cache-maintenance flags for v3d_g2d_run.  PRE: pre-job invalidation
 * (drop stale V3D L2/slice lines so the QPU sees fresh DRAM data).
 * POST: post-job flush (drain the TMU write combiner and clean the L2
 * so the job's writes reach DRAM).  A standalone dispatch needs both;
 * back-to-back bands of one large-surface op skip PRE on all but the
 * first band and POST on all but the last - the maps are row
 * independent and no CPU access happens between bands, so the
 * intermediate full-L2 walks are redundant.  PRE-elided dispatches
 * still refresh the uniform block visibility (the QPU uniform fetch is
 * served through the L2T/slice caches and would otherwise re-read the
 * previous dispatch's stale uniform block). */
#define V3D_G2D_MAINT_PRE  (1u << 0)
#define V3D_G2D_MAINT_POST (1u << 1)
#define V3D_G2D_MAINT_ALL  (V3D_G2D_MAINT_PRE | V3D_G2D_MAINT_POST)

/*
 * Run one CSD dispatch of `code` with `unifs` against the surfaces
 * `src`/`dst` (either may be NULL/0).  src/dst are VIRTUAL addresses;
 * the caller has already substituted physical addresses into the uniform
 * fields that describe them.  This wrapper keeps the ARM/V3D caches
 * coherent around the dispatch (nothing for NOCACHE dma canvases),
 * bounded by the maint flags.
 * Returns 0 on success.
 */
int v3d_g2d_run(const uint64_t *code, int nwords,
                const uint32_t *unifs, int nunifs,
                int num_qpus,
                const void *src, size_t src_len,
                void *dst, size_t dst_len, unsigned maint);

/*
 * VC4 (V3D 2.1) counterpart of v3d_g2d_run(): launches one SRQ thread
 * per QPU of `code`.  The uniform stream is PER-QPU: QPU q reads its
 * contract words starting at unifs + q * VC4_UNIF_QWORDS (32-word
 * slots; words beyond the kernel contract are ignored).  Returns 0 on
 * success, 1 on launch timeout, -1 on a bad argument.
 */
#define VC4_UNIF_QWORDS 32
int v3d_g2d_run_vc4(const uint64_t *code, int nwords,
                    const uint32_t *unifs, int num_qpus,
                    const void *src, size_t src_len,
                    void *dst, size_t dst_len);

/* The ARGB8888 kernels (see g2d_qpu_kernels.h): the CSD kernels are
 * 1:1 binary translations of the raspi5 (V3D 7.1) kernels to the V3D
 * 4.2 ISA; *_vc4 are VC4 (V3D 2.1) SRQ kernels. */
extern const uint64_t g2d_qpu_argb_fill[];
extern const unsigned g2d_qpu_argb_fill_n;
extern const uint64_t g2d_qpu_argb_blit[];
extern const unsigned g2d_qpu_argb_blit_n;
extern const uint64_t g2d_qpu_argb_rotate[];
extern const unsigned g2d_qpu_argb_rotate_n;
extern const uint64_t g2d_qpu_argb_rot90[];
extern const unsigned g2d_qpu_argb_rot90_n;
extern const uint64_t g2d_qpu_argb_alpha[];
extern const unsigned g2d_qpu_argb_alpha_n;
extern const uint64_t g2d_qpu_argb_scale_pow2[];
extern const unsigned g2d_qpu_argb_scale_pow2_n;
extern const uint64_t g2d_qpu_argb_copy[];
extern const unsigned g2d_qpu_argb_copy_n;
extern const uint64_t g2d_qpu_argb_fill4[];
extern const unsigned g2d_qpu_argb_fill4_n;
extern const uint64_t g2d_qpu_argb_fill_vc4[];
extern const unsigned g2d_qpu_argb_fill_vc4_n;
extern const uint64_t g2d_qpu_argb_blit_vc4[];
extern const unsigned g2d_qpu_argb_blit_vc4_n;
extern const uint64_t g2d_qpu_argb_rotate_vc4[];
extern const unsigned g2d_qpu_argb_rotate_vc4_n;
extern const uint64_t g2d_qpu_argb_alpha_vc4[];
extern const unsigned g2d_qpu_argb_alpha_vc4_n;

#endif /* V3D_G2D_H */
