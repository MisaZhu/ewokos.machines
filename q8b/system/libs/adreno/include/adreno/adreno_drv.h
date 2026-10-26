/*
 * adreno_drv.h - internal shared device context for the Q8B Adreno 690 driver.
 *
 * One adreno_dev_t instance per process (g2dd / displayd each own one).  It
 * holds the mapped MMIO state, the SMMU context, the GMU state, the CP
 * ringbuffer and the small set of GPU-resident scratch buffers every submission
 * needs.  All fields are filled by adreno_a6xx_init() in bring-up order:
 *
 *   reg map -> SMMU CB0 + pagetable -> GMU boot -> CP ring -> SQE/zap -> ready
 *
 * The sequences mirror Linux drivers/gpu/drm/msm/adreno/{a6xx_gmu,a6xx_gpu,
 * adreno_gpu}.c (v6.6) and drivers/iommu/arm/arm-smmu/.  Register offsets come
 * from the Mesa-derived a6xx_regs.h / a6xx_gmu_regs.h; SMMU offsets from
 * arm-smmu.h.  Nothing here is hardware-validated on SC8280XP yet - see the
 * bring-up note in adreno_a6xx.c.
 */
#ifndef ADRENO_DRV_H
#define ADRENO_DRV_H

#include <stdint.h>
#include <ewoksys/ewokdef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A GPU-resident buffer: an uncached dma_alloc pool slice with both its CPU
   virtual address and the physical address.  For CB0 (GPU) buffers the SMMU is
   identity-mapped so pa == the GPU virtual address.  For GMU (CB1) buffers the
   firmware fixes the iova (icache @0x4000, dummy @0x60000000, debug
   @0x60400000), so `iova` carries the GMU-visible address separately. */
typedef struct {
	void        *va;
	uint64_t     pa;
	uint64_t     iova;   /* GMU/CB1 virtual address (== pa for CB0 buffers) */
	uint32_t     size;
} adreno_bo_t;

/* One CP ringbuffer (we use a single kernel ring, RB_ID 0, like Linux RB1). */
typedef struct {
	adreno_bo_t  bo;          /* the ring storage                       */
	uint32_t     sizedwords;  /* ring size in dwords (power of two)     */
	volatile uint32_t wptr;   /* driver-side write pointer (dwords)     */
	uint64_t     rptr_gpu;    /* last observed GPU read pointer         */
	adreno_bo_t  scratch;     /* fence/timestamp memory (CACHE_FLUSH_TS)*/
	adreno_bo_t  ib;          /* reusable indirect-buffer staging       */
	uint64_t     fence_iova;  /* GPU addr of the fence dword (scratch[0])*/
} adreno_ring_t;

typedef struct {
	/* --- SMMU (adreno_smmu.c) --- */
	int      smmu_ready;      /* CB0 (GPU) identity map programmed       */
	int      smmu_gmu_ready;/* CB1 (GMU) firmware map programmed         */
	uint32_t pgshift;         /* 12 (4K) or 16 (64K), from GR0_ID1      */
	uint32_t numpage;         /* global pages before CB0, from ID1      */
	uint32_t num_cb;          /* context banks, from GR0_ID1.NUMCB      */
	uint32_t cb0_page;        /* page index of CB0 = numpage + 0        */
	adreno_bo_t pgtable;      /* CB0 LPAE stage-1 root (L1), identity   */
	adreno_bo_t cb0_pt_pool;  /* CB0 L2/L3 tables for privileged pages */
	adreno_bo_t gmu_pt_pool;  /* bump pool for CB1 L1/L2/L3 tables      */
	adreno_bo_t gmu_pgtable;  /* CB1 LPAE stage-1 root (L1)             */
	uint64_t ttbr0;           /* programmed CB0 TTBR0 value (PA | ASID) */
	uint64_t gmu_ttbr0;       /* programmed CB1 TTBR0 value             */

	/* --- GMU (adreno_gmu.c) --- */
	int      gmu_booted;
	adreno_bo_t gmu_icache;   /* GMU icache image, iova 0x4000, 16M-16K */
	adreno_bo_t gmu_dummy;    /* GMU dummy page,  iova 0x60000000, 8K   */
	adreno_bo_t gmu_debug;    /* GMU debug buf,   iova 0x60400000, 28K  */
	adreno_bo_t gmu_log;      /* GMU event log,   16K (driver iova)     */
	adreno_bo_t gmu_hfi;      /* HFI queue table + queues, 16K          */
	uint32_t hfi_seq;         /* HFI sequence counter                   */

	/* --- CP / ring (adreno_ring.c) --- */
	int      cp_ready;
	int      wedged;          /* a submitted job never retired / faulted:
	                             every later submit declines instantly   */
	adreno_ring_t ring;
	adreno_bo_t rptr_shadow;  /* privileged HW-written RB read pointer */

	/* --- GPU core (adreno_a6xx.c) --- */
	int      gpu_ready;       /* hw_init done, submissions accepted     */
	int      zap_done;        /* zap shader authenticated + reset       */
	adreno_bo_t sqe_bo;       /* SQE microcode staged for CP_SQE load   */
	uint32_t fence;           /* monotonic fence/timestamp counter      */

	/* --- init state --- */
	int      init_rc;         /* 0 ready, <0 decline (sticky)           */
	int      init_tried;
} adreno_dev_t;

/* The single process-wide device instance (adreno_a6xx.c owns it). */
adreno_dev_t* adreno_dev(void);

/* Runtime opt-in/config parser used by g2dd and fixed platform settings. */
int adreno_flag(const char *name);
int adreno_cfg_int(const char *name, int def);

int  adreno_rpmh_interconnect_vote(void);
int  adreno_rpmh_scm_vote(void);
int  adreno_rpmh_arc_vote(const char *id, uint32_t level, uint32_t *vote);

int  adreno_wdt_arm(unsigned seconds);
void adreno_wdt_disable(void);

/* Allocate a GPU-visible uncached buffer from the dma pool.  Returns 0 on
   success (bo->va/pa/size filled), negative on failure.  block_id 0 is the
   shared sys_dma pool the q8b BSP already uses (see xhci.c). */
int  adreno_bo_alloc(adreno_bo_t *bo, uint32_t size);
void adreno_bo_free(adreno_bo_t *bo);

/* --- module entry points (each in its own .c) --- */
int adreno_gpucc_init(void);            /* GMU/hub clock enables (adreno_gpucc.c) */
int adreno_gpucc_validate_runtime(void); /* verify vote-controlled NoC clocks */
/* Enable GPU_CC_GX_GMU_CLK.  Must run only after GPU_SET OOB has made the GMU
   power the GX rail; enabling it earlier leaves the branch halted because its
   power domain is off. */
int adreno_gpucc_gx_clk_enable(void);

/* Vote the RPMh "gfx.lvl" power domain (the GPUCC's parent domain in the DT).
   Must run before adreno_gpucc_init(): it mirrors gpucc-sc8280xp.c probe()'s
   pm_runtime_get_sync() and is what brings the GPU NoC ports out of collapse.
	Returns 0 when every required vote was accepted, negative otherwise. */
int adreno_rpmh_gfx_vote(void);
int adreno_smmu_init(adreno_dev_t *d);
void adreno_smmu_dump_fault(adreno_dev_t *d);
/* Program CB1 (the GMU context bank) with the firmware-fixed iova map for the
   GMU BOs already allocated in d (icache/dummy/debug/log/hfi).  Must run after
   adreno_smmu_init() and after the GMU BOs are allocated. */
int adreno_smmu_gmu_cb_init(adreno_dev_t *d);
int adreno_gmu_init(adreno_dev_t *d);
int adreno_gmu_gpu_set(void);
void adreno_gmu_gpu_clear(void);
int adreno_ring_init(adreno_dev_t *d);
/* Program the CP engine onto the ring allocated by adreno_ring_init() and run
   the a6xx_cp_init() CP_ME_INIT handshake.  Must run AFTER adreno_a6xx.c has
   written CP_RB_BASE / CP_RB_CNTL / CP_SQE_INSTR_BASE / CP_SQE_CNTL=1. */
int adreno_ring_cp_init(adreno_dev_t *d);
int adreno_ring_exit_secure(adreno_dev_t *d);
int adreno_a6xx_init(adreno_dev_t *d);   /* top-level: calls the above */

/* Result codes shared by adreno_ring_submit and every adreno_2d_* /
   adreno_3d_* op built on it.  The split matters to the bsp layer: a job that
   was never handed to the GPU may be replayed on the CPU, a job that WAS
   submitted and did not retire must not be (the GPU may still be writing the
   surface) and latches the device wedged (fail-fast, no speculative reset -
   the port has no validated a6xx recovery sequence; see ring_mark_wedged). */
#define ADRENO_OK            0
#define ADRENO_E_DECLINED  (-1)   /* rejected before any GPU write          */
#define ADRENO_E_TIMEOUT   (-2)   /* submitted; ring/fence never retired    */
#define ADRENO_E_FAULT     (-3)   /* submitted; RBBM fault status latched   */
#define ADRENO_E_WEDGED    (-4)   /* declined: device already wedged        */

/* Submit one indirect buffer (the dword stream in `cmds`) on the ring and wait
   for its fence to retire.  Serialized by an internal parking lock: g2dd runs
   concurrent ipc workers and the IB staging buffer, ring write pointer and
   fence are shared, so submissions are strictly one at a time, including the
   completion wait. Runtime completion shares a 1 s wall-clock budget,
   with a brief spin followed by parking. Returns one of the ADRENO_* codes. */
int adreno_ring_submit(adreno_dev_t *d, const uint32_t *cmds, uint32_t dwords);
/* Bounded runtime diagnostics, disabled by default. No GX MMIO reads. */
void adreno_ring_trace(uint32_t submissions);
int adreno_ring_tracing(void);
/* Serialized submit statistics and bounded wait-policy diagnostics.
 * "submit gpu 1|0" enables optional CP timestamp/cycle copies around each IB;
 * reports GPU execution ns and CP cycle/reference-clock ratio as cp_khz.
 * GPU sampling defaults off and is independent of CPU phase profiling. */
int adreno_ring_cmd(int argc, char **argv, char *buf, uint32_t len);
/* Non-zero once a submitted job failed to retire or faulted (sticky). */
int adreno_wedged(void);

/* Top-level 2D / 3D entry points consumed by bsp_g2d.c / bsp_g3d.c. */
int adreno_2d_init(void);
int adreno_3d_init(void);

/* a6xx compute-shader (CS) 3D back end (adreno_3d.c).  Loads a precompiled
   Adreno program blob (adreno_prog.h VERSION 2, the a6xx ir3 contract - NOT
   the V3D-QPU g2d_pgl_program_blob_t) and dispatches it either as a fullscreen
   compute kernel that writes the framebuffer (is_geom == 0) or as the two-pass
   compute geometry rasterizer (is_geom != 0: kern[1] vertex transform into a
   daemon-owned `vout` buffer, then kern[0] rasterizes from it).  Physical
   addresses are the GPU-visible base (CB0 is identity-mapped so pa == iova);
   the framebuffer is uint32 (bound as a linear R32_UINT buffer view) with
   fb_stride the byte row pitch (multiple of 4, base 64-byte aligned).
   adreno_3d_program_load returns a >=0 handle or a negative value when the
   blob is not a valid Adreno program, the GPU is not up, the program table is
   full, or a BO allocation failed.  adreno_3d_draw returns ADRENO_OK,
   ADRENO_E_DECLINED when the handle is bad or the draw is ineligible (e.g. it
   needs an SSBO binding the request does not carry; nothing was submitted),
   or ADRENO_E_TIMEOUT / ADRENO_E_FAULT / ADRENO_E_WEDGED from the submission -
   the caller surfaces that and the client falls back to its in-process
   engine.  There is no CPU path here.  See adreno_3d.c for the numbered
   hardware-validation gaps. */
int adreno_3d_ready(void);
/* Diagnostic submit A/B: 0 separate geometry passes, 1 batch (default),
 * -1 query. Affects the next draw, never changes an in-flight submission. */
int adreno_3d_geom_batch(int enabled);

/* Exact g2d nearest sampling. endpoints selects Q15 corner-preserving
   SCALE_TO; zero selects exact rational cropped BLIT sampling. */
int adreno_compute_scale(uint64_t src_phy, int32_t src_w, int32_t src_h,
                         int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                         uint64_t dst_phy, int32_t dst_w, int32_t dst_h,
                         int32_t dx, int32_t dy, int32_t dw, int32_t dh,
                         int endpoints);
int adreno_compute_blend(uint64_t src_phy, int32_t src_w, int32_t src_h,
                         int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                         uint64_t dst_phy, int32_t dst_w, int32_t dst_h,
                         int32_t dx, int32_t dy, int32_t dw, int32_t dh,
                         uint8_t alpha);
int adreno_compute_fill_alpha(uint64_t dst_phy, int32_t dst_w, int32_t dst_h,
                             int32_t x, int32_t y, int32_t w, int32_t h,
                             uint32_t color);
int adreno_compute_rotate(uint64_t src_phy, int32_t src_w, int32_t src_h,
                          uint64_t dst_phy, int32_t dst_w, int32_t dst_h,
                          int32_t degree, int32_t cos_q14, int32_t sin_q14);
int adreno_3d_program_load(const void *blob, uint32_t size);
int adreno_3d_program_free(int32_t handle);

/* Geometry-draw inputs for adreno_3d_draw; ignored (may be NULL) when the
   program is a plain fullscreen kernel.  Mirrors what raspi5's v3d_3d_draw
   feeds its draw_compute_geom: the vertex streams and depth canvas ride the
   client's own physical buffers, ctex unit 0 is the linear uint32 texture
   image (textured kinds 2/4) or the per-frame instance-matrix SSBO (kind 3).
   `mode` is the G2D_PGL_MODE_* code (only TRIANGLES is dispatchable). */
typedef struct {
	uint32_t mode;
	uint32_t nverts;
	uint64_t pos_phy;      /* interleaved vertex stream (kind-specific stride) */
	uint64_t col_phy;      /* float3 normals (gouraud / instanced), else 0     */
	uint64_t z_phy;        /* uint32 depth canvas, fb_w x fb_h                 */
	uint32_t z_stride;     /* depth row pitch, bytes                            */
	uint64_t ctex_phy;     /* ctex unit 0 base (texture image / instance SSBO)  */
	uint32_t ctex_stride;  /* unit 0 row pitch, bytes                           */
	uint32_t ctex_w;       /* unit 0 width, texels (or SSBO dwords per row)    */
	uint32_t ctex_h;       /* unit 0 height, rows                               */
	int      first_frame;  /* clear fb to clear_rgba + depth to ~0 first       */
	uint32_t clear_rgba;   /* 0xAABBGGRR, portablegl byte order                 */
} adreno_3d_geom_t;

/* Sampled-texture units for the fullscreen (is_geom==0) path: one entry per
   ctex unit the compiled program binds (unit i -> texture_index i).  All are
   linear RGBA8 images uploaded by the client's adreno_texlayout.c, so `tiling`
   is always 0 (PGLC_TEX_TILING_RASTER) and `phy` is the GPU-visible base.
   `ub_pad`'s high nibble carries the filter (G2D_PGL_CTEX_FILTER_*: 0=NEAREST,
   1=LINEAR).  For a cubemap `depth` is the layer count (6) and `arrstride64`
   the per-layer size in 64-byte units (layer L lives at phy + L*arrstride64*64);
   a plain 2D texture has depth<=1.  An entry with phy==0 is unbound and the
   daemon substitutes a 1x1 dummy so the slot samples safely. */
typedef struct {
	uint64_t phy;
	uint32_t stride;       /* row pitch, bytes                                  */
	uint32_t w;            /* width, texels                                     */
	uint32_t h;            /* height, texels                                    */
	uint8_t  tiling;       /* 0 = linear raster (the only upload q8b produces)  */
	uint8_t  ub_pad;       /* high nibble = G2D_PGL_CTEX_FILTER_*               */
	uint8_t  depth;        /* cubemap layer count (6), else <=1                 */
	uint8_t  reserved;
	uint32_t arrstride64;  /* per-layer size in 64-byte units (cubemap)         */
} adreno_3d_tex_t;

int adreno_3d_draw(uint64_t fb_phy, int32_t fb_w, int32_t fb_h,
                   uint32_t fb_stride, int32_t prog_handle,
                   const adreno_3d_geom_t *geom,
                   const adreno_3d_tex_t *tex, uint32_t num_tex,
                   const uint32_t *unif, uint32_t num_uniforms);

/* OpenCL / Vulkan compute offload (the bsp_cl route, mirroring raspi5's
   vc_cl -> v3d_3d_cl_dispatch).  Dispatches a pure-compute program loaded
   through adreno_3d_program_load (is_geom == 0: the CL / Vulkan-compute blob
   the app-side adrenocompiler serialized) over a wg_count[0] x [1] x [2]
   work-group grid.  The __global buffer / SSBO arguments arrive as already-
   resolved contig physical bases in binding order (arg i -> the GLSL
   binding i the transpiler emitted; CB0 is identity-mapped so pa == iova,
   zero copy), arg_size[i] giving each buffer's byte length (a 0 phy/size
   marks an unused trailing slot); `unif` carries num_uniforms u32 of the
   by-name packed scalar-argument uniform blob (UBO 0).  The work-group shape
   is baked into the compiled kernel, so wg_size must equal the kernel's
   local_size product.  Returns ADRENO_OK, ADRENO_E_DECLINED when the handle
   is bad, the program is not a pure-compute one, the launch shape does not
   match the kernel or an argument is not GPU-bindable (nothing submitted),
   or ADRENO_E_TIMEOUT / ADRENO_E_FAULT / ADRENO_E_WEDGED from the
   submission.  A submitted dispatch is never replayed. */
int adreno_3d_cl_dispatch(int32_t prog_handle,
                          const uint32_t wg_count[3], uint32_t wg_size,
                          const uint64_t *arg_phy, const uint32_t *arg_size,
                          uint32_t num_args,
                          const uint32_t *unif, uint32_t num_uniforms);

/* A2D 2D-blitter back end (adreno_2d.c).  All surfaces are uint32 ARGB
   (FMT6_8_8_8_8_UNORM, linear).  Physical addresses are the GPU-visible base
   (CB0 is identity-mapped so pa == iova); pitch is in bytes and must be
   64-byte aligned (the A2D pitch fields are shr6).  Each returns ADRENO_OK,
   ADRENO_E_DECLINED when the GPU is not up or the op is ineligible (nothing
   was submitted), or ADRENO_E_TIMEOUT / ADRENO_E_FAULT / ADRENO_E_WEDGED from
   the submission - the caller maps those to the g2d result codes and the
   client falls back to its in-process engine.  There is no CPU path here. */
int adreno_2d_ready(void);
int adreno_2d_fill(uint64_t dst_phy, int32_t dst_w, int32_t dst_h,
                   uint32_t dst_pitch, int32_t x, int32_t y, int32_t w,
                   int32_t h, uint32_t color, int rb_swap);
int adreno_2d_copy(uint64_t src_phy, int32_t src_w, int32_t src_h,
                   uint32_t src_pitch, int32_t sx, int32_t sy, int32_t sw,
                   int32_t sh, uint64_t dst_phy, int32_t dst_w, int32_t dst_h,
                   uint32_t dst_pitch, int32_t dx, int32_t dy, int32_t dw,
                   int32_t dh, int32_t degree);
/* Per-pixel source-over blend of a src rect onto dst (RB_A2D_PIXEL_CNTL =
   PIXEL_OP_BLENDING, SRC_ALPHA / ONE_MINUS_SRC_ALPHA).  Blends by the source
   pixel alpha only; the caller must restrict it to a global alpha of 0xff
   (it cannot express a src.a * global_alpha product).  See adreno_2d.c gap
   #6: a colour blend through the A2D is register-documented but not
   Mesa-demonstrated, so it is unvalidated on silicon. */
int adreno_2d_blend(uint64_t src_phy, int32_t src_w, int32_t src_h,
                    uint32_t src_pitch, int32_t sx, int32_t sy, int32_t sw,
                    int32_t sh, uint64_t dst_phy, int32_t dst_w, int32_t dst_h,
                    uint32_t dst_pitch, int32_t dx, int32_t dy, int32_t dw,
                    int32_t dh);

/* Gaussian-style blur of a canvas sub-rect, in place, via a bilinear scale
   cascade (mip blur).  The A2D has no convolution op, so this halves the rect
   L times (L ~ log2(radius)) into a scratch mip chain with the documented
   bilinear FILTER on BLIT_OP_SCALE and upsamples it back into the rect - a
   real GPU blur that approximates, but does not bit-match, the reference
   gaussian (see adreno_2d.c gap #7). Radius 1..64 selects up to six levels;
   strength is quantized to powers of two and limited by the rect dimensions.
   The complete chain uses one bounded IB/fence; per-stage clean/idle and
   invalidation keep intermediate texture reads coherent.
   canvas_phy/W/H/canvas_pitch describe the
   whole canvas (pitch 64-aligned); (x,y,w,h) is the clamped sub-rect;
   tmp_phy/tmp_limit are the 64-aligned contiguous scratch base and the byte
   budget the caller guarantees (g2dd: (h-1)*W*4 + w*4).  The canvas is written
   ONLY by the final upsample, so any earlier failure leaves it pristine.  Both
   surfaces are uint32 ARGB (FMT6_8_8_8_8_UNORM, linear).  Returns ADRENO_OK,
   ADRENO_E_DECLINED when the GPU is not up, the op is ineligible or the
   scratch is too small (canvas unchanged; scratch may have been written), or the submission's
   ADRENO_E_TIMEOUT / ADRENO_E_FAULT / ADRENO_E_WEDGED - the caller maps those
   to terminal g2d errors, retaining buffers without replay until cold boot.
   There is no CPU path here. */
int adreno_2d_blur(uint64_t canvas_phy, int32_t W, int32_t H,
                   uint32_t canvas_pitch, int32_t x, int32_t y, int32_t w,
                   int32_t h, int32_t radius, uint64_t tmp_phy,
                   uint64_t tmp_limit);

#ifdef __cplusplus
}
#endif

#endif /* ADRENO_DRV_H */
