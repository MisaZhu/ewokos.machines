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

/* V3D clock rate in Hz confirmed during initialization, or 0 if unknown. */
uint32_t v3d_g2d_clock_hz(void);

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

/*
 * Run one CSD dispatch of `code` with `unifs` against the surfaces
 * `src`/`dst` (either may be NULL/0).  src/dst are VIRTUAL addresses;
 * the caller has already substituted physical addresses into the uniform
 * fields that describe them.  This wrapper keeps the ARM/V3D caches
 * coherent around the dispatch (nothing for NOCACHE dma canvases).
 * Returns 0 on success.
 *
 * v3d_g2d_run knows no physical extents, so its V3D L2T maintenance
 * walks the WHOLE cache before and after the job.  v3d_g2d_run_ranged
 * additionally takes the physical bases of the same two surfaces
 * (src_phy/dst_phy, 0 when unknown) and bounds the walks to the uniform
 * block plus those canvases - the per-dispatch fixed cost then scales
 * with the surface size instead of the 4 GB tag space.  Every 2D caller
 * holds those bases (it put them in the uniforms), so prefer the ranged
 * entry; the plain one is the safe default for a caller without them.
 * `devcmd /dev/g2d maint full` restores whole-L2 walks at runtime.
 */
int v3d_g2d_run(const uint64_t *code, int nwords,
                const uint32_t *unifs, int nunifs,
                int num_qpus,
                const void *src, size_t src_len,
                void *dst, size_t dst_len);
int v3d_g2d_run_ranged(const uint64_t *code, int nwords,
                       const uint32_t *unifs, int nunifs,
                       int num_qpus,
                       const void *src, size_t src_len, uint32_t src_phy,
                       void *dst, size_t dst_len, uint32_t dst_phy);

/* Runtime policy knobs (devcmd /dev/g2d maint range|full, spinus <us>).
 * maint_ranged: bounded L2T walks (default on) vs whole-L2 per dispatch.
 * spin_us: wall-clock window the completion wait busy-polls before it
 * parks in usleep(500) slices (default 1000; 0 parks immediately). */
void v3d_g2d_set_maint_ranged(int on);
int v3d_g2d_maint_ranged(void);
void v3d_g2d_set_spin_us(uint32_t us);
uint32_t v3d_g2d_spin_us(void);

/*
 * Generic CSD dispatch of a mesa-compiled COMPUTE shader (the soft3d
 * compute-geometry route: v3d_3d.c stages the compiled QPU code and the
 * expanded uniform stream in dma and references them by PHYSICAL address -
 * raspix has no V3D MMU page table, unlike raspi5's IOVA model, so the
 * code_iova/unif_iova/dst_phy/aux_phy/src_phy fields below are physical
 * addresses here).  Neither the code nor the uniforms are staged by this
 * call.  The CFG words follow mesa's v3dx launch_grid encoding: one
 * supergroup per workgroup when wg_size is a multiple of 16; cfg[4] is
 * NUM_BATCHES-1 on V3D 4.2 and the raw batch count on V3D 7.1 (IP rev>=6).
 * dst/aux bound the surfaces the POST L2 writeback must cover (the geom
 * rasterizer's atomicMin depth canvas is the aux surface); the POST walk
 * is bounded to them when both are given (whole-L2 otherwise).  The PRE
 * walk stays whole-L2 on raspix: the compiled code is rewritten per job
 * and the job does not describe its extent.  Same return contract as
 * v3d_g2d_run: 0 done, 1 timeout, -1 fault / bad arguments / V3D 2.1.
 */

/* Run flags for v3d_g2d_run_csd.  PRE: pre-job clean+invalidate of the
 * V3D L2T/slice caches over the job's ARM-written source extent
 * (src_phy/src_span; whole-L2 when the extent is absent or the maint knob
 * is off) so the QPU sees fresh DRAM data.  The uniform stream
 * (unif_iova/unif_span) is always dropped from the caches, PRE or not.
 * The compiled code is NOT part of the per-dispatch PRE: it is uploaded
 * once per program and v3d_g2d_l2_invalidate_all() at upload drops any
 * lines a previous occupant of the same dma block left behind.  POST:
 * post-job flush (drain the TMU write combiner and clean the L2T) so the
 * job's writes - including the aux depth surface - reach DRAM before the
 * CPU reads or re-clears them.  POLL_YIELD / POLL_IRQ are accepted for
 * API compatibility: every wait spins the spinus window first and parks
 * in usleep(500) slices past it (an immediate park costs a whole
 * scheduler tick, ~976us, even for a sub-100us transform pass).
 * budget_ms bounds the wait in wall-clock ms. */
#define V3D_G2D_MAINT_PRE  (1u << 0)
#define V3D_G2D_MAINT_POST (1u << 1)
#define V3D_G2D_MAINT_ALL  (V3D_G2D_MAINT_PRE | V3D_G2D_MAINT_POST)
#define V3D_G2D_POLL_YIELD (1u << 2)
#define V3D_G2D_POLL_IRQ   (1u << 3)

typedef struct {
    uint32_t code_iova;     /* compiled compute QPU code (physical)      */
    uint32_t unif_iova;     /* expanded uniform stream (physical)        */
    uint32_t wg_count[3];   /* dispatch grid in workgroups               */
    uint32_t wg_size;       /* local_size product, 1..256                */
    uint8_t  threads;       /* prog_data threads: 1, 2 or 4              */
    uint8_t  single_seg;    /* prog_data single_seg                      */
    uint32_t dst_phy;       /* destination surface base (POST), or 0     */
    uint32_t dst_span;      /* destination surface bytes, or 0           */
    uint32_t aux_phy;       /* second GPU-written surface (POST), or 0   */
    uint32_t aux_span;      /* aux surface bytes, or 0                   */
    uint32_t src_phy;       /* PRE source extent base, or 0              */
    uint32_t src_span;      /* PRE source extent bytes, or 0             */
    uint32_t unif_span;     /* expanded uniform stream bytes, or 0       */
    uint32_t budget_ms;     /* completion wait budget; 0 = driver default */
    uint8_t  tag;           /* stat lane (V3D_G2D_TAG_*), 0 = untagged   */
} v3d_g2d_csd_job_t;

/* Stat lanes for csd_job_t::tag: every compute dispatch lands in the csd
 * lane AND in the tag_* accumulator its tag names, so the stat line can
 * split the geom passes (gx = pass-1 transform, gr = pass-2 raster) from
 * the single-pass compute route (cs).  An out-of-range tag counts as NONE. */
enum {
    V3D_G2D_TAG_NONE = 0,
    V3D_G2D_TAG_GEOM_XFORM,
    V3D_G2D_TAG_GEOM_RASTER,
    V3D_G2D_TAG_COMPUTE,
    V3D_G2D_TAG_N
};

int v3d_g2d_run_csd(const v3d_g2d_csd_job_t *job, unsigned flags);

/* V3D 4.2 fixed-function TLB clear probe.  Returns -2 when unsupported. */
int v3d_g2d_render_clear(uint32_t dst_phys, uint32_t stride_bytes,
                         uint32_t width, uint32_t height, uint32_t argb);

/* V3D 4.2 fixed-function TLB raster-to-raster copy.  This is intentionally
 * restricted to equal-stride, whole-surface ARGB8888 copies; callers use the
 * QPU path for clipping, scaling and format conversion. */
int v3d_g2d_render_copy(uint32_t src_phys, uint32_t dst_phys,
                        uint32_t stride_bytes,
                        uint32_t width, uint32_t height);

/* Nanosecond breakdown of the most recent synchronous CSD submission.  This is
 * diagnostic state only: reading it does not alter cache or dispatch policy. */
typedef struct {
    uint64_t total;
    uint64_t prepare;
    uint64_t invalidate;
    uint64_t execute;
    uint64_t flush;
    uint32_t invalidate_polls;
    uint32_t execute_polls;
    uint32_t flush_polls;
    uint32_t perf[16];
} v3d_g2d_profile_t;

void v3d_g2d_get_profile(v3d_g2d_profile_t *out);

/* Whole-L2T + slice-cache clean+invalidate under the dispatch lock.  For
 * the one-off ARM writes the per-dispatch ranged PRE does not describe:
 * v3d_3d.c calls it after uploading a program's QPU code so a dma block
 * reused from a freed program cannot serve the old instructions. */
void v3d_g2d_l2_invalidate_all(void);

/* Op bracket for multi-launch operations: holds the dispatch lock across
 * every v3d_g2d_run / v3d_g2d_run_vc4 / staging call of one op so another
 * worker's op cannot interleave between its batches (the lock is
 * owner-recursive; the dispatches inside nest).  Must be paired on every
 * return path; a single-dispatch op needs no bracket. */
void v3d_g2d_op_begin(void);
void v3d_g2d_op_end(void);

/* Per-dispatch phase accounting, always on (a few fine-clock reads per
 * dispatch, no syscall).  One dispatch is one CSD job (V3D 4.2 QPU
 * kernel), one SRQ launch (VC4) or one TLB render job (V3D 4.2
 * render_clear / render_copy); csd/srq/tlb split runs by queue.
 * pre = staging + V3D cache invalidate up to the kick; exec = kick to
 * completion (a timeout counts its full poll budget); post = the L2
 * flush after completion (CSD only; the SRQ launch flushes inside its
 * own wait and the TLB stores bypass the L2T).  pre_full/post_full count
 * the dispatches whose PRE/POST walked the WHOLE L2T (no physical extent
 * known, knob off, V3D 2.1); a healthy 2D or geom stream on V3D 4.2 shows
 * both at 0 (the single-pass compute route keeps a full PRE: its texture
 * set has no single extent).  parks counts the usleep(500) slices the
 * completion waits spent off the core past the spin window.  The tag_*
 * arrays split the compute dispatches by v3d_g2d_csd_job_t::tag. */
typedef struct {
    uint64_t runs;
    uint64_t csd, srq, tlb;
    uint64_t timeouts;
    uint64_t pre_ns, exec_ns, post_ns;
    uint64_t pre_full, post_full;
    uint64_t parks;
    uint64_t tag_runs[V3D_G2D_TAG_N];
    uint64_t tag_pre_ns[V3D_G2D_TAG_N];
    uint64_t tag_exec_ns[V3D_G2D_TAG_N];
    uint64_t tag_post_ns[V3D_G2D_TAG_N];
    uint64_t tag_parks[V3D_G2D_TAG_N];
} v3d_g2d_stats_t;

/* Snapshot the counters (consistent: taken under the dispatch lock) and
 * optionally zero them. */
void v3d_g2d_stats_read(v3d_g2d_stats_t *out, int reset);

/*
 * VC4 (V3D 2.1) counterpart of v3d_g2d_run(): launches one SRQ thread
 * per QPU of `code`.  The uniform stream is PER-QPU: QPU q reads its
 * contract words starting at unifs + q * VC4_UNIF_QWORDS (words beyond
 * the kernel contract are ignored).  Returns 0 on success, 1 on launch
 * timeout, -1 on a bad argument.
 *
 * This stride is the per-launch cost that every VC4 dispatch pays:
 * both staging paths copy nq * VC4_UNIF_QWORDS words from the caller's
 * array into DMA memory and the ring consumes the same amount per
 * launch.  It is sized by the widest per-QPU contract in the tree, which
 * is the alpha pipeline's staging contracts; the self-looping copy
 * kernel reads s[0..6].  32 * 4 = 128 bytes is also a multiple of
 * VC4_STAGING_ALIGN, so every per-QPU block starts on the alignment the
 * SRQUA register requires.
 *
 * Shrinking it was tried (32 -> 16) while only the span-shaped kernels
 * were in the tree and their widest contract was 4 words; keep the
 * headroom so a wider future contract fails the v3d_g2d.c guard instead
 * of reading a neighbour's words.
 */
#define VC4_UNIF_QWORDS 32
int v3d_g2d_run_vc4(const uint64_t *code, int nwords,
                    const uint32_t *unifs, int num_qpus,
                    const void *src, size_t src_len,
                    void *dst, size_t dst_len);

/* VC4 only: evict the BCM2837 system L3 before a public operation starts
 * reading caller-owned memory.  Call once before the operation's first SRQ
 * dispatch; a multi-batch operation must not repeat the 384 KiB scrub for
 * every batch.  Returns 0 on success and -1 when preparation cannot run. */
int v3d_g2d_vc4_prepare_reads(void);

/* VC4 only: allocate short-lived GPU-visible dispatch data from the
 * driver's reusable staging arena.  The returned address remains valid
 * until the next allocation wraps the arena; completed SRQ batches are
 * retired and the cache scrub runs before any physical address is reused. */
void *v3d_g2d_vc4_staging_alloc(size_t bytes, uint32_t *phys_out);

/* VC4 (V3D 2.1): stage `code` into the proven per-launch fetch region
 * (_run_code) and return its physical launch address for SRQPC, or 0 on
 * error.  Pi3 bring-up: on BCM2837 the QPU cannot reliably fetch the
 * init-time dma staging regions (stale 0x55555555 data, SRQ wedges), so
 * every VC4 dispatch must launch from this region.  Call before
 * v3d_g2d_vc4_run() with a code you did not get from the preloaded
 * kernel table. */
uint32_t v3d_g2d_vc4_stage_at(const uint64_t *code, int nwords, int off);
#define v3d_g2d_vc4_stage(code, nwords) v3d_g2d_vc4_stage_at((code), (nwords), 0)

/* Real-silicon diagnostic helper: launch code previously copied with
 * v3d_g2d_vc4_stage(), using the normal driver's SRQ/cache discipline.
 * This intentionally does not accept an arbitrary code pointer: callers
 * must stage into the driver's proven fetch region first. */
int v3d_g2d_vc4_run_staged(uint32_t code_p, const uint32_t *unifs,
                           int num_qpus, uint32_t *srqcs_out);

/* The ARGB8888 kernels (see g2d_qpu_kernels.h): *_fill/_blit/_rotate/
 * _alpha are V3D 4.2 CSD kernels, *_vc4 are VC4 (V3D 2.1) SRQ
 * kernels. */
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
extern const uint64_t g2d_qpu_argb_fill_vc4[];
extern const unsigned g2d_qpu_argb_fill_vc4_n;
extern const uint64_t g2d_qpu_argb_fill_loop_vc4[];
extern const unsigned g2d_qpu_argb_fill_loop_vc4_n;
extern const uint64_t g2d_qpu_argb_blit_vc4[];
extern const unsigned g2d_qpu_argb_blit_vc4_n;
extern const uint64_t g2d_qpu_argb_rotate_vc4[];
extern const unsigned g2d_qpu_argb_rotate_vc4_n;
extern const uint64_t g2d_qpu_argb_alpha_vc4[];
extern const unsigned g2d_qpu_argb_alpha_vc4_n;
extern const uint64_t g2d_qpu_argb_gather_vc4[];
extern const unsigned g2d_qpu_argb_gather_vc4_n;
extern const uint64_t g2d_qpu_argb_alpha_gather_vc4[];
extern const unsigned g2d_qpu_argb_alpha_gather_vc4_n;
/* Self-looping VC4 copy kernel: one launch per 12 destination rows for
 * integer Q15 maps.  Dispatched only under the G2D_BAND_MAX_VDW
 * per-thread iteration cap. */
extern const uint64_t g2d_qpu_argb_copy_loop_vc4[];
extern const unsigned g2d_qpu_argb_copy_loop_vc4_n;
/* Self-looping VC4 alpha kernel: copy-loop shape plus a dst TMU gather
 * and the source-over ALU blend - one launch per 12 destination rows
 * under the same eligibility and iteration cap as the copy loop. */
extern const uint64_t g2d_qpu_argb_alpha_loop_vc4[];
extern const unsigned g2d_qpu_argb_alpha_loop_vc4_n;

#endif /* V3D_G2D_H */
