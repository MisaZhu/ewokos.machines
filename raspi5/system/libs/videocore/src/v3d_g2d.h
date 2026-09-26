/*
 * v3d_g2d.h - VideoCore VII (V3D) hardware back end for the EwokOS
 * raspberry-pi5 bsp_g2d layer.
 *
 * Bring-up maps the V3D register block (0x1002000000, outside the main
 * MMIO window, whitelisted by the raspi5 kernel check_mem_map_arch) and
 * the PM power domain into the caller's address space through
 * SYS_MEM_MAP, then power-cycles the GRAFX_V3D domain and enables the
 * L2 cache.  CSD code/uniform staging lives in physically-contiguous
 * sys_dma buffers (dma_alloc).  A V3D-local page table limits QPU access
 * to the DMA and contiguous-shm windows, including their >4GB locations.
 *
 * Canvases arrive as virtual addresses (shmat() / dma window addresses)
 * with their resolved physical base supplied by the caller (the bsp_g2d
 * *_phy parameters); v3d_g2d_run() maintains the ARM/V3D caches around
 * the dispatch and the kernels use page-table-mapped V3D IOVAs.
 *
 * ZERO COPY: the GPU never copies canvas pixels.  The kernels write and
 * read the caller's buffers in place through their V3D IOVAs;
 * the only memory the driver touches is the (small) uniform block, which
 * is refreshed per call.  The CSD kernels are preloaded once at init, so
 * a dispatch copies nothing but the uniforms.
 */

#ifndef V3D_G2D_H
#define V3D_G2D_H

#include <stdint.h>
#include <stddef.h>
#include <ewoksys/ewokdef.h>

/* Map registers + allocate dma staging; idempotent.  Returns 0 when the
 * GPU is usable and a real failure otherwise.  Operations outside the
 * documented GPU capability set may still use the CPU compatibility path. */
int v3d_g2d_init(void);

/* Non-zero once the GPU is usable. */
int v3d_g2d_ready(void);

/* V3D clock rate in Hz as confirmed at init (0 when unknown/unsupported,
 * e.g. the property mailbox was unavailable). */
uint32_t v3d_g2d_clock_hz(void);

/* Number of logical CSD batches used by the kernels, derived from the
 * hardware's slice and QPU counts. */
int v3d_g2d_num_qpus(void);

/* V3D IOVA of the TMU write-scratch surface the kernels use for out-of-rect
 * writes and the flush epilogue (feed it to uniforms). */
uint32_t v3d_g2d_scratch_phys(void);

/* Address validation gate: returns non-zero when [phy, phy+bytes) lies
 * inside a region installed in the V3D-local page table (the sys_dma
 * window or IPC_CONTIG shm slab).  Every
 * caller-supplied *_phy must pass this check before dispatch. */
int v3d_g2d_phy_valid(ewokos_addr_t phy, size_t bytes);

/* One-shot hardware probe of the vec4 (TMUC general-access) TMU path
 * used by the argb_copy/argb_fill4 kernels: runs a small GPU copy into
 * scratch and verifies it on the CPU.  Non-zero when the fast vec4
 * kernels are usable; callers must fall back to the single-word kernels
 * otherwise.  The result is cached after the first call. */
int v3d_g2d_vec4_ok(void);

/* Run flags for v3d_g2d_run.  PRE: pre-job invalidation
 * (drop stale V3D L2/slice lines so the QPU sees fresh DRAM data) plus
 * the ARM-side clean of the sources.  POST: post-job flush (drain the
 * TMU write combiner and clean the L2 so the job's writes reach DRAM)
 * plus the ARM-side invalidate of the destination.  A standalone
 * dispatch needs both; back-to-back bands of one large-surface op skip
 * PRE on all but the first band and POST on all but the last - the
 * maps are row independent and no CPU access happens between bands,
 * so the intermediate full-L2 walks are redundant (a failed band still
 * leaves any dirty lines to the next dispatch's PRE, whose mode-0
 * clean+invalidate writes them back first, so nothing is lost).
 * PRE-elided dispatches are NOT free of ordering work: v3d_g2d_run
 * still runs a uniform-visibility barrier (g2d_uniform_fresh: dsb plus
 * a ranged clean+invalidate of just the 256-byte uniform block and a
 * slice invalidate), because the QPU uniform fetch is served through
 * the L2T/slice caches and would otherwise re-read the previous
 * dispatch's stale uniform block - eliding it made every middle band
 * re-render band 0 (measured on silicon).  POLL_YIELD tells the CSD-done
 * loop that the caller predicts a >1 ms dispatch, so it may yield one
 * scheduler frame with proc_usleep(0) between register polls. */
#define V3D_G2D_MAINT_PRE  (1u << 0)
#define V3D_G2D_MAINT_POST (1u << 1)
#define V3D_G2D_MAINT_ALL  (V3D_G2D_MAINT_PRE | V3D_G2D_MAINT_POST)
#define V3D_G2D_POLL_YIELD (1u << 2)

/*
 * Run one CSD dispatch of `code` with `unifs` against the surfaces
 * `src`/`dst` (either may be NULL/0).  src/dst are VIRTUAL addresses;
 * the caller has already substituted V3D IOVAs into the uniform fields
 * that describe them.  This wrapper keeps the ARM/V3D caches
 * coherent around the dispatch (dc civac/ivac for cacheable canvases,
 * nothing for NOCACHE dma canvases), bounded by the run flags.
 * Returns 0 on success.
 */
int v3d_g2d_run(const uint64_t *code, int nwords,
                const uint32_t *unifs, int nunifs,
                int num_qpus,
                const void *src, size_t src_len,
                void *dst, size_t dst_len, unsigned flags);

/* The ARGB8888 CSD kernels (assembled from the .qpu sources). */
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

/* Separable Gaussian blur passes, radius 2 (h5+v5) and radius 4 (h9+v9):
 * hardware-verified encodings, see g2d_qpu_kernels.h. */
extern const uint64_t g2d_qpu_gauss_h5[];
extern const unsigned g2d_qpu_gauss_h5_n;
extern const uint64_t g2d_qpu_gauss_v5[];
extern const unsigned g2d_qpu_gauss_v5_n;
extern const uint64_t g2d_qpu_gauss_h9[];
extern const unsigned g2d_qpu_gauss_h9_n;
extern const uint64_t g2d_qpu_gauss_v9[];
extern const unsigned g2d_qpu_gauss_v9_n;

#endif /* V3D_G2D_H */
