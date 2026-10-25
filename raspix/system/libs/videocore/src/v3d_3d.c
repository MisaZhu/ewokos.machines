/*
 * v3d_3d.c - VideoCore (V3D) COMPUTE-ONLY 3D back end for the EwokOS raspix
 * videocore library.  See v3d_3d.h for the public contract.
 *
 * This is the compute-only subset of raspi5's v3d_3d.c.  raspix ships ONE
 * binary across Pi3 (VC4 V3D 2.1) / Pi4 (V3D 4.2) / Pi5 (V3D 7.1), and the
 * only GPU raster route that works on this silicon is the CSD compute
 * geometry rasterizer (the V3D binner/render pipeline and the hand-written
 * fixed-function shaders are NOT ported: on raspi5 they are compile-disabled
 * via V3D_3D_ENABLE_FIXED_DRAW and never complete a bin).  So this module
 * keeps ONLY:
 *
 *   - the compiled-program cache (v3d_3d_program / _free): the QPU code and
 *     the static uniform value streams are deep-copied into dma and the
 *     prog_data cached by handle;
 *   - the QUNIFORM interpreter (expand_uniforms), version-independent because
 *     the enum values come from the SAME vendored mesa the app-process
 *     compiler emits them with;
 *   - draw_compute (the single-pass shadertoy full-frame route) and
 *     draw_compute_geom (the two-pass geometry rasterizer: cs2 vertex xform
 *     -> vout, cs raster -> fb + atomicMin depth);
 *   - v3d_3d_draw routing: a compiled program carrying a compute stage
 *     dispatches through v3d_g2d_run_csd; EVERY other request declines so the
 *     portablegl client keeps its CPU/NEON rasterizer.
 *
 * Adaptation to raspix (vs raspi5):
 *   - PHYSICAL addresses, not IOVAs: raspix has no V3D MMU page table, so the
 *     code/uniform/surface addresses handed to v3d_g2d_run_csd are physical
 *     (the field names keep raspi5's *_iova spelling so vc_pgl3d.c ports
 *     verbatim; alloc_stage's dma_phy_addr already yields a physical address
 *     that v3d_g2d_phy_valid gates);
 *   - dispatch goes through v3d_g2d_run_csd (Phase B), which owns the CSD
 *     register programming, the cache maintenance and the completion wait for
 *     ver 42 and 71 and declines ver 21;
 *   - the frame clear uses the 2D gpu_fill entry (V3D 4.2 routes a full-surface
 *     fill through the proven fixed-function TLB clear) instead of a binner RCL;
 *   - there is no wedge/recover latch on raspix, so the v3d_g2d_wedged() gate
 *     and the binner-only v3d_g2d_core_regs()/l2_flush_all() calls are absent.
 *
 * Every hardware wait is bounded inside v3d_g2d_run_csd; a submitted job is
 * NEVER replayed - a timeout degrades to the caller's CPU path.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ewoksys/dma.h>
#include <ewoksys/klog.h>
#include <videocore/vc_g2d.h>
#include "v3d_g2d.h"
#include "v3d_3d.h"

/* ---- staging sizes ---------------------------------------------------- */
#define V3D_3D_COMPUTE_BUDGET_MS 2000u     /* compiled compute draw wait    */

/* Two-pass geometry rasterizer intermediate: `vout` holds one packed record per
 * vertex, written by the pass-1 vertex-transform kernel and read by the pass-2
 * raster kernel.  The gouraud / textured / instanced variants pack 8 floats
 * (32 bytes); the colour-modulated textured variant (cs_is_geom == 4, imgui)
 * also carries a per-vertex colour, so its record is 12 floats (48 bytes).  The
 * buffer is sized for the widest record so every variant fits.  Sized for
 * V3D_3D_MAX_VERTS vertices; a single process-lifetime buffer is safe because
 * CSD dispatches are serialized (v3d_g2d_op_begin) and each geom draw fully
 * overwrites the sub-range it consumes. */
#define V3D_3D_VOUT_BYTES (V3D_3D_MAX_VERTS * 48u)

/* WS4 compiled-shader sampler bindings: per-unit V3D71 TEXTURE_SHADER_STATE and
 * SAMPLER_STATE records, each in its own 32-byte slot (V3D_3D_TEXSTATE_SLOT) so
 * the TMU_CONFIG low bits OR cleanly.  Only the single-pass draw_compute route
 * samples through the TMU; the geometry rasterizer reads its texture/instance
 * data as a raw SSBO and never builds these.  The encodings below are V3D 7.1
 * (the shadertoy route is a Pi5 target); a V3D 4.2 textured compute draw would
 * need the 4.2 record layout, which is out of scope for the geometry apps. */
#define V3D_3D_TEXSTATE_SLOT 32u

/* ---- dma staging (VA + physical), allocated once at init -------------- */
static uint32_t *_ccu_unif_va; static uint32_t _ccu_unif_p;/* compiled COMPUTE expanded uniforms */
static uint32_t *_vout_va;     static uint32_t _vout_p;    /* geom pass-1 -> pass-2 records */
static uint8_t  *_texstate_va; static uint32_t _texstate_p;/* 4 texture records */
static uint8_t  *_sampler_va;  static uint32_t _sampler_p; /* 4 sampler records */

static int _ready;

/* ---- compiled-program cache (WS4) ---------------------------------- */
/* Each uploaded program's GPU-resident physical addresses (VS/FS/coord/cs/cs2
 * QPU + static uniform value streams) plus its CPU-side prog_data (uniform
 * layout descriptors, VPM sizes, attribute/input maps, FS flags) are cached
 * here by handle.  The compute route references only the cs/cs2 stages, but the
 * VS/FS/coord fields are kept (and staged) so the shared client blob and the
 * vc_pgl3d bridge stay byte-for-byte compatible with raspi5. */
typedef struct {
    int      used;
    /* GPU-resident QPU code (dma physical + word count) */
    uint32_t vs_qpu_iova;   uint32_t vs_qpu_words;
    uint32_t fs_qpu_iova;   uint32_t fs_qpu_words;
    uint32_t coord_qpu_iova; uint32_t coord_qpu_words; /* binning coord shader */
    /* raw dma_alloc VAs of the QPU code blocks (0 when absent) so
     * v3d_3d_program_free can hand them back; the addresses above point into
     * the page-rounded interior of these blocks and cannot be freed directly. */
    ewokos_addr_t vs_qpu_raw, fs_qpu_raw, coord_qpu_raw, cs_qpu_raw;
    /* GPU-resident static uniform value streams (dma physical); 0 when the
     * stage has no uniforms.  A per-draw buffer is patched from these at draw. */
    uint32_t vs_unif_iova;  uint32_t vs_num_uniforms;
    uint32_t fs_unif_iova;  uint32_t fs_num_uniforms;
    uint32_t coord_unif_iova; uint32_t coord_num_uniforms;
    /* CPU-side uniform layout descriptors (how to build per-draw uniforms) */
    uint32_t vs_unif_contents[V3D_3D_PROG_MAX_UNIF];
    uint32_t fs_unif_contents[V3D_3D_PROG_MAX_UNIF];
    uint32_t coord_unif_contents[V3D_3D_PROG_MAX_UNIF];
    /* CPU-side per-slot uniform ARGS (the mesa `uniforms[]` stream).  The
     * QUNIFORM interpreter reads these + the per-draw GL uniform blob at draw
     * time and expands each slot into ONE u32 in the shared per-draw dma
     * staging (_ccu_unif); the raw arg stream is never handed to the GPU. */
    uint32_t vs_unif_data[V3D_3D_PROG_MAX_UNIF];
    uint32_t fs_unif_data[V3D_3D_PROG_MAX_UNIF];
    uint32_t coord_unif_data[V3D_3D_PROG_MAX_UNIF];
    /* vertex-stage prog_data */
    uint8_t  vs_threads, vs_single_seg;
    uint32_t vs_vpm_input_size, vs_vpm_output_size, vs_vcm_cache_size;
    uint8_t  vs_separate_segments;
    uint8_t  vs_vattr_sizes[V3D_3D_PROG_MAX_ATTRS / 4];
    int32_t  vs_driver_location_map[V3D_3D_PROG_MAX_ATTRS];
    /* fragment-stage prog_data */
    uint8_t  fs_threads, fs_single_seg;
    uint8_t  fs_needs_coords, fs_uses_center_w, fs_writes_z, fs_disable_ez;
    uint32_t fs_num_inputs;
    uint32_t fs_input_slots[V3D_3D_PROG_MAX_INPUTS];
    /* coordinate-stage prog_data */
    uint8_t  coord_threads, coord_single_seg;
    uint32_t coord_vpm_input_size, coord_vpm_output_size, coord_vcm_cache_size;
    uint8_t  coord_separate_segments;
    /* optional compute stage (shadertoy / geom raster); cs_qpu_words == 0 when
     * absent.  Dispatched through v3d_g2d_run_csd. */
    uint32_t cs_qpu_iova;   uint32_t cs_qpu_words;
    uint32_t cs_num_uniforms;
    uint32_t cs_unif_contents[V3D_3D_PROG_MAX_UNIF];
    uint32_t cs_unif_data[V3D_3D_PROG_MAX_UNIF];
    uint8_t  cs_threads, cs_single_seg;
    uint32_t cs_local_size[3];
    uint32_t cs_shared_size;
    uint32_t cs_is_geom;    /* 1 = gouraud geom, 2 = textured, 3 = instanced,
                             * 4 = colour-modulated textured (imgui) */
    uint32_t cs_geom_tpw;   /* raster triangles/workgroup: 0 = one wg per tri */
    /* second compute stage (cs2 = pass-1 vertex transform of the two-pass
     * geometry rasterizer).  cs2_qpu_words == 0 when absent.  Dispatched FIRST
     * (one invocation per vertex) writing the packed `vout` records the cs
     * raster pass then reads.  cs2_uniform_blob_dwords splits the per-draw
     * uniform blob: [0, cs2_uniform_blob_dwords) feeds cs2, the rest feeds cs. */
    uint32_t cs2_qpu_iova;  uint32_t cs2_qpu_words;
    ewokos_addr_t cs2_qpu_raw;
    uint32_t cs2_num_uniforms;
    uint32_t cs2_unif_contents[V3D_3D_PROG_MAX_UNIF];
    uint32_t cs2_unif_data[V3D_3D_PROG_MAX_UNIF];
    uint8_t  cs2_threads, cs2_single_seg;
    uint32_t cs2_local_size[3];
    uint32_t cs2_shared_size;
    uint32_t cs2_uniform_blob_dwords;
} v3d_3d_prog_t;

static v3d_3d_prog_t _progs[V3D_3D_MAX_PROGS];

/* ------------------------------------------------------------------ */
/* dma staging allocation: dma_alloc is not page-aligned, so over-       */
/* allocate and round the VA up to a page (the contiguous dma mapping    */
/* keeps phys page-aligned too).  Mirrors v3d_g2d.c's staging alloc.     */
/* ------------------------------------------------------------------ */
static int alloc_stage_raw(uint32_t size, void **va, uint32_t *iova,
                           ewokos_addr_t *raw)
{
    ewokos_addr_t r = dma_alloc(0, size + 4095u);
    ewokos_addr_t v, p;

    if (r == 0)
        return -1;
    v = (r + 4095u) & ~(ewokos_addr_t)4095u;
    p = dma_phy_addr(0, v);
    /* the buffer must be inside a RAM range the GPU may address */
    if (p == 0 || (p >> 32) != 0 || !v3d_g2d_phy_valid(p, size)) {
        dma_free(0, r);
        return -1;
    }
    *va = (void *)(uintptr_t)v;
    *iova = (uint32_t)p;
    if (raw != NULL)
        *raw = r;
    return 0;
}

/* permanent staging (never freed): the raw block handle is not needed */
static int alloc_stage(uint32_t size, void **va, uint32_t *iova)
{
    return alloc_stage_raw(size, va, iova, NULL);
}

/* hand a program's QPU code blocks back to dma and clear the slot */
static void release_prog(v3d_3d_prog_t *pr)
{
    if (pr->vs_qpu_raw)    dma_free(0, pr->vs_qpu_raw);
    if (pr->fs_qpu_raw)    dma_free(0, pr->fs_qpu_raw);
    if (pr->coord_qpu_raw) dma_free(0, pr->coord_qpu_raw);
    if (pr->cs_qpu_raw)    dma_free(0, pr->cs_qpu_raw);
    if (pr->cs2_qpu_raw)   dma_free(0, pr->cs2_qpu_raw);
    memset(pr, 0, sizeof(*pr));
}

/* ------------------------------------------------------------------ */
/* WS4 compiled-shader sampler state records (single-pass compute route). */
/* ------------------------------------------------------------------ */

/* Set the little-endian bit field [lo..hi] (inclusive; bit 0 == rec[0] bit
 * 0) of a packed 24-byte record to val, mirroring mesa's util_bitpack_uint
 * so the layout matches the V3D71_*_pack functions exactly. */
static void rec_set_bits(uint8_t *rec, uint32_t lo, uint32_t hi, uint32_t val)
{
    uint32_t b;
    for (b = lo; b <= hi; b++) {
        uint8_t mask = (uint8_t)(1u << (b & 7u));
        if ((val >> (b - lo)) & 1u)
            rec[b >> 3] |= mask;
        else
            rec[b >> 3] &= (uint8_t)~mask;
    }
}

/* Pack a single-level, non-array RGBA8 V3D71_TEXTURE_SHADER_STATE (24 bytes)
 * sampling the w x h image at base_iova (which must be 64-byte aligned). */
static void pack_tex_state_rgba8(uint8_t *rec, uint32_t base_iova,
                                 uint32_t w, uint32_t h,
                                 uint32_t tiling, uint32_t ub_pad)
{
    uint32_t width  = w ? w - 1u : 0u;
    uint32_t height = h ? h - 1u : 0u;
    uint32_t uif = (tiling == V3D_3D_TEX_TILING_UIF_NO_XOR ||
                    tiling == V3D_3D_TEX_TILING_UIF_XOR);

    memset(rec, 0, 24);
    rec_set_bits(rec,   0,  31, base_iova);   /* texture_base_pointer      */
    rec_set_bits(rec,  58,  71, width);       /* image_width  = w - 1      */
    rec_set_bits(rec,  72,  85, height);      /* image_height = h - 1      */
    rec_set_bits(rec,  86,  99, 0u);          /* image_depth  = 0 (2D)     */
    rec_set_bits(rec, 100, 106, 4u);          /* texture_type = RGBA8      */
    rec_set_bits(rec, 107, 107, uif);         /* extended (level-0 fields) */
    rec_set_bits(rec, 108, 110, 2u);          /* swizzle_r = RED           */
    rec_set_bits(rec, 111, 113, 3u);          /* swizzle_g = GREEN         */
    rec_set_bits(rec, 114, 116, 4u);          /* swizzle_b = BLUE          */
    rec_set_bits(rec, 117, 119, 5u);          /* swizzle_a = ALPHA         */
    rec_set_bits(rec, 120, 123, 0u);          /* max_level  = 0            */
    rec_set_bits(rec, 124, 127, 0u);          /* base_level = 0            */
    if (uif) {
        rec_set_bits(rec, 128, 131, ub_pad & 0xFu);  /* level_0_ub_pad     */
        rec_set_bits(rec, 132, 132,                  /* level_0_xor_enable */
                     tiling == V3D_3D_TEX_TILING_UIF_XOR ? 1u : 0u);
        rec_set_bits(rec, 134, 134, 1u);             /* level_0_is_strictly_uif */
    }
    rec_set_bits(rec, 135, 135, 0u);          /* uif_xor_disable = 0       */
}

/* Pack a GL_NEAREST + GL_REPEAT V3D71_SAMPLER_STATE (24 bytes). */
static void pack_sampler_state_nearest(uint8_t *rec)
{
    memset(rec, 0, 24);
    rec_set_bits(rec,  0,  0, 1u);   /* mag_filter_nearest        */
    rec_set_bits(rec,  1,  1, 1u);   /* min_filter_nearest        */
    rec_set_bits(rec,  2,  2, 1u);   /* mip_filter_nearest        */
    rec_set_bits(rec,  3,  3, 0u);   /* anisotropy_enable         */
    rec_set_bits(rec,  4,  6, 0u);   /* depth_compare_function    */
    rec_set_bits(rec,  7,  7, 1u);   /* srgb_disable              */
    rec_set_bits(rec,  8, 19, 0u);   /* min_level_of_detail = 0.0 */
    rec_set_bits(rec, 20, 31, 0u);   /* max_level_of_detail = 0.0 */
    rec_set_bits(rec, 32, 47, 0u);   /* fixed_bias = 0.0          */
    rec_set_bits(rec, 48, 50, 0u);   /* wrap_s = REPEAT           */
    rec_set_bits(rec, 51, 53, 0u);   /* wrap_t = REPEAT           */
    rec_set_bits(rec, 54, 56, 0u);   /* wrap_r = REPEAT           */
    rec_set_bits(rec, 57, 57, 0u);   /* wrap_i_border             */
    rec_set_bits(rec, 58, 60, 0u);   /* border_color_mode         */
    rec_set_bits(rec, 61, 62, 0u);   /* maximum_anisotropy        */
}

/* Build the four identical sampler records once. */
static void build_sampler_records(void)
{
    uint32_t u;
    for (u = 0; u < V3D_3D_MAX_TEX_UNITS; u++)
        pack_sampler_state_nearest(_sampler_va + u * V3D_3D_TEXSTATE_SLOT);
}

/* Rebuild the per-unit texture records from this draw's ctex bindings.  A
 * unit with ctex_iova == 0 stays unbound (zeroed record).  Returns 0 on
 * success, -1 if a bound unit's pixel data is not 64-byte aligned or is still
 * in raster layout (the TMU cannot sample it; the caller then declines). */
static int build_tex_state_records(const v3d_3d_ctex_t *ctex)
{
    uint32_t u;
    if (ctex == NULL)
        return 0;
    for (u = 0; u < V3D_3D_MAX_TEX_UNITS; u++) {
        uint8_t *rec = _texstate_va + u * V3D_3D_TEXSTATE_SLOT;
        if (ctex->ctex_iova[u] == 0) {
            memset(rec, 0, 24);
            continue;
        }
        if ((ctex->ctex_iova[u] & 0x3Fu) != 0)
            return -1;
        if (ctex->ctex_tiling[u] == V3D_3D_TEX_TILING_RASTER ||
            ctex->ctex_tiling[u] > V3D_3D_TEX_TILING_UIF_XOR) {
            slog("v3d_3d: ctex unit %u tiling %u unsupported -> decline\r\n",
                 u, (uint32_t)ctex->ctex_tiling[u]);
            return -1;
        }
        pack_tex_state_rgba8(rec, ctex->ctex_iova[u],
                             ctex->ctex_w[u], ctex->ctex_h[u],
                             ctex->ctex_tiling[u], ctex->ctex_ub_pad[u]);
    }
    return 0;
}

int v3d_3d_init(void)
{
    void *va;
    uint32_t iova;

    if (_ready)
        return V3D_3D_DONE;
    if (!v3d_g2d_ready())
        return V3D_3D_UNSUPPORTED;
    /* VC4 (V3D 2.1, Pi3) has no CSD compute engine: decline so the client
     * keeps its CPU/NEON rasterizer. */
    if (v3d_g2d_ver() == 21)
        return V3D_3D_UNSUPPORTED;

    /* compiled compute-stage expanded-uniform staging: the CSD fetches the
     * expanded stream straight from here (cfg[6]). */
    if (alloc_stage(V3D_3D_PROG_MAX_UNIF * 4u, &va, &iova) != 0)
        return V3D_3D_UNSUPPORTED;
    _ccu_unif_va = (uint32_t *)va; _ccu_unif_p = iova;

    /* two-pass geometry rasterizer intermediate (see _vout_va above) */
    if (alloc_stage(V3D_3D_VOUT_BYTES, &va, &iova) != 0)
        return V3D_3D_UNSUPPORTED;
    _vout_va = (uint32_t *)va; _vout_p = iova;

    /* sampler state records for the single-pass textured compute route: 4
     * texture + 4 sampler records, each in its own 32-byte slot. */
    if (alloc_stage(V3D_3D_MAX_TEX_UNITS * V3D_3D_TEXSTATE_SLOT, &va, &iova) != 0)
        return V3D_3D_UNSUPPORTED;
    _texstate_va = (uint8_t *)va; _texstate_p = iova;

    if (alloc_stage(V3D_3D_MAX_TEX_UNITS * V3D_3D_TEXSTATE_SLOT, &va, &iova) != 0)
        return V3D_3D_UNSUPPORTED;
    _sampler_va = (uint8_t *)va; _sampler_p = iova;
    build_sampler_records();

    __asm__ __volatile__("dsb sy" ::: "memory");

    _ready = 1;
    slog("v3d_3d: compute staging ready (ver %d ccu %u vout %u)\r\n",
         v3d_g2d_ver(), _ccu_unif_p, _vout_p);
    return V3D_3D_DONE;
}

int v3d_3d_ready(void)
{
    return _ready;
}

/* ------------------------------------------------------------------ */
/* mesa QUNIFORM interpreter.                                            */
/* ------------------------------------------------------------------ */

/* mesa's NIR->QPU compiler emits, per uniform slot, a `contents` descriptor
 * (enum quniform_contents, mesa/src/broadcom/compiler/v3d_compiler.h) plus a
 * 32-bit arg.  At draw time the gallium driver expands each slot into ONE u32
 * the QPU pulls with ldunifrf.  These are the leading enum entries and are
 * stable across the pinned mesa; because the SAME vendored mesa compiles the
 * program (in the app process) for ver 42 and ver 71, the values the daemon
 * decodes here match by construction on every board. */
#define QUNIFORM_CONSTANT          0u
#define QUNIFORM_UNIFORM           1u
#define QUNIFORM_VIEWPORT_X_SCALE  2u
#define QUNIFORM_VIEWPORT_Y_SCALE  3u
#define QUNIFORM_VIEWPORT_Z_OFFSET 4u
#define QUNIFORM_VIEWPORT_Z_SCALE  5u
#define QUNIFORM_TMU_CONFIG_P0     6u
#define QUNIFORM_TMU_CONFIG_P1     7u
#define QUNIFORM_SSBO_OFFSET       19u  /* data = SSBO binding; compute route */

static uint32_t f32_bits(float f)
{
    uint32_t u;
    __builtin_memcpy(&u, &f, 4);
    return u;
}

/* V3D clipper XY granularity (mesa v3d_device_info.c: 64 for ver 71, 256 for
 * ver 42): the Xs/Ys VPM words and CLIPPER_XY_SCALING are in 1/granularity px.
 * The compute route passes vp_*==0 (its kernels do their own viewport transform
 * in-shader via uViewportWH), so this only matters if a viewport QUNIFORM is
 * ever expanded; kept version-correct for safety. */
static float clipper_xy_granularity(void)
{
    return (v3d_g2d_ver() >= 71) ? 64.0f : 256.0f;
}

/* Expand one stage's uniform stream into `out` (num u32s).  contents[]/data[]
 * are the cached CPU descriptors+args; job_unif (num_job u32s, may be NULL) is
 * the per-draw GL uniform storage a QUNIFORM_UNIFORM slot indexes; vp_* are the
 * viewport transform values; ctex carries this draw's per-unit texture bindings
 * (may be NULL); ssbo_iova (num_ssbo entries, may be NULL) resolves
 * QUNIFORM_SSBO_OFFSET by binding.  Returns 0 on success, -1 if any slot's
 * contents is outside the handled subset or an index is out of range (the caller
 * then declines rather than feeding the QPU a wrong value). */
static int expand_uniforms(uint32_t *out, const uint32_t *contents,
                           const uint32_t *data, uint32_t num,
                           const uint32_t *job_unif, uint32_t num_job,
                           const v3d_3d_ctex_t *ctex,
                           float vp_xscale, float vp_yscale,
                           float vp_zscale, float vp_zoff,
                           const uint32_t *ssbo_iova, uint32_t num_ssbo)
{
    uint32_t i;
    for (i = 0; i < num; i++) {
        uint32_t c = contents[i] & 0xFFu;   /* the enum value (defensive mask) */
        uint32_t d = data[i];
        switch (c) {
        case QUNIFORM_CONSTANT:
            out[i] = d;                     /* the arg IS the value */
            break;
        case QUNIFORM_UNIFORM:
            if (job_unif == NULL || d >= num_job) {
                slog("v3d_3d: unif slot %u GL uniform[%u] out of %u -> decline\r\n",
                     i, d, num_job);
                return -1;                  /* per-draw GL uniform[d] */
            }
            out[i] = job_unif[d];
            break;
        case QUNIFORM_VIEWPORT_X_SCALE:
            out[i] = f32_bits(vp_xscale * clipper_xy_granularity());
            break;
        case QUNIFORM_VIEWPORT_Y_SCALE:
            out[i] = f32_bits(vp_yscale * clipper_xy_granularity());
            break;
        case QUNIFORM_VIEWPORT_Z_SCALE:
            out[i] = f32_bits(vp_zscale);
            break;
        case QUNIFORM_VIEWPORT_Z_OFFSET:
            out[i] = f32_bits(vp_zoff);
            break;
        case QUNIFORM_TMU_CONFIG_P0: {
            uint32_t unit = d >> 24;
            /* The single-pass compute route (draw_compute) samples through the
             * TMU using the V3D71-encoded TEXTURE_SHADER_STATE / SAMPLER_STATE
             * records built at init.  V3D 4.2 (Pi4) uses a different texture-
             * state layout, so a TMU-sampling compute program cannot be bound
             * correctly there: decline it and the client keeps its CPU
             * rasterizer.  This gate never fires for the geometry rasterizer -
             * its kernels read textures/instance data as raw SSBOs
             * (QUNIFORM_SSBO_OFFSET), not the TMU - so the textured geom route
             * (3ddemo, cs_is_geom==2) still runs on V3D 4.2. */
            if (v3d_g2d_ver() < 71) {
                slog("v3d_3d: unif slot %u TMU P0 on ver %d (<71) -> decline\r\n",
                     i, v3d_g2d_ver());
                return -1;
            }
            if (ctex == NULL || unit >= V3D_3D_MAX_TEX_UNITS ||
                ctex->ctex_iova[unit] == 0) {
                slog("v3d_3d: unif slot %u TMU P0 unit %u unbound -> decline\r\n", i, unit);
                return -1;
            }
            out[i] = (_texstate_p + unit * V3D_3D_TEXSTATE_SLOT) | (d & 0xFu);
            break;
        }
        case QUNIFORM_TMU_CONFIG_P1: {
            uint32_t unit = d >> 24;
            if (v3d_g2d_ver() < 71) {   /* see the P0 case: V3D71 records only */
                slog("v3d_3d: unif slot %u TMU P1 on ver %d (<71) -> decline\r\n",
                     i, v3d_g2d_ver());
                return -1;
            }
            if (ctex == NULL || unit >= V3D_3D_MAX_TEX_UNITS ||
                ctex->ctex_iova[unit] == 0) {
                slog("v3d_3d: unif slot %u TMU P1 unit %u unbound -> decline\r\n", i, unit);
                return -1;
            }
            out[i] = (_sampler_p + unit * V3D_3D_TEXSTATE_SLOT) | (d & 0x7u);
            break;
        }
        case QUNIFORM_SSBO_OFFSET:
            /* Compute route: mesa resolves this slot to a bound SSBO's GPU
             * address.  The compiler already remapped the slot data from the
             * linker block index to the declared BINDING, so index the caller's
             * ssbo_iova[] table by it. */
            if (ssbo_iova == NULL || d >= num_ssbo || ssbo_iova[d] == 0) {
                slog("v3d_3d: unif slot %u SSBO binding %u unbound -> decline\r\n", i, d);
                return -1;
            }
            out[i] = ssbo_iova[d];
            break;
        default:
            slog("v3d_3d: unif slot %u type %u data 0x%x unhandled -> decline\r\n",
                 i, c, d);
            return -1;
        }
    }
    return 0;
}

/* Compiled-draw decline: name the gate so the client's bare -2 is
 * attributable from /dev/log (the client latches to CPU after the first). */
static int decline(const char *why)
{
    slog("v3d_3d: compiled draw declined: %s\r\n", why);
    return V3D_3D_UNSUPPORTED;
}

/* Compute-route draw: the program's fragment stage was recompiled as a compute
 * kernel whose SSBO binding 0 is the render target.  One invocation per pixel:
 * the grid is ceil(w/lx) x ceil(h/ly) x 1 and the kernel clips x>=w / y>=h
 * itself.  Serialized against the 2D CSD kernels by the dispatch lock inside
 * v3d_g2d_run_csd. */
static int draw_compute(const v3d_3d_prog_t *pr, uint32_t fb_iova,
                        uint32_t stride_bytes, uint32_t fmt,
                        uint32_t w, uint32_t h,
                        const uint32_t *unif, uint32_t num_uniforms,
                        const v3d_3d_ctex_t *ctex)
{
    v3d_g2d_csd_job_t job;
    int rc;

    if (fmt != V3D_3D_FMT_RGBA8)
        return decline("compute: fmt not RGBA8");
    if ((stride_bytes & 3u) != 0 || stride_bytes < w * 4u)
        return decline("compute: stride");
    if (pr->cs_local_size[0] == 0 || pr->cs_local_size[1] == 0 ||
        pr->cs_local_size[2] != 1u)
        return decline("compute: local_size");
    if (pr->cs_shared_size != 0)
        return decline("compute: shared memory unsupported");

    v3d_g2d_op_begin();
    if (build_tex_state_records(ctex) != 0) {
        v3d_g2d_op_end();
        return decline("compute: ctex base not 64B aligned");
    }
    if (expand_uniforms(_ccu_unif_va, pr->cs_unif_contents, pr->cs_unif_data,
                        pr->cs_num_uniforms, unif, num_uniforms, ctex,
                        0.0f, 0.0f, 0.0f, 0.0f, &fb_iova, 1u) != 0) {
        v3d_g2d_op_end();
        return decline("compute: uniforms");
    }
    __asm__ __volatile__("dsb sy" ::: "memory");

    memset(&job, 0, sizeof(job));
    job.code_iova   = pr->cs_qpu_iova;
    job.unif_iova   = _ccu_unif_p;
    job.wg_count[0] = (w + pr->cs_local_size[0] - 1u) / pr->cs_local_size[0];
    job.wg_count[1] = (h + pr->cs_local_size[1] - 1u) / pr->cs_local_size[1];
    job.wg_count[2] = 1u;
    job.wg_size     = pr->cs_local_size[0] * pr->cs_local_size[1];
    job.threads     = pr->cs_threads;
    job.single_seg  = pr->cs_single_seg;
    job.dst_phy     = fb_iova;
    job.dst_span    = stride_bytes * h;
    job.unif_span   = pr->cs_num_uniforms * 4u;
    job.tag         = V3D_G2D_TAG_COMPUTE;
    /* a ray-marching effect at 640x480 legitimately runs for hundreds of ms;
     * give it a real budget instead of the 2D fail-fast default */
    job.budget_ms   = V3D_3D_COMPUTE_BUDGET_MS;
    rc = v3d_g2d_run_csd(&job, V3D_G2D_MAINT_ALL | V3D_G2D_POLL_YIELD);
    v3d_g2d_op_end();
    if (rc != 0) {
        slog("v3d_3d: compute draw rc=%d grid %ux%u wg=%u thr=%u unif=%u\r\n",
             rc, job.wg_count[0], job.wg_count[1], job.wg_size,
             (uint32_t)pr->cs_threads, pr->cs_num_uniforms);
        return rc > 0 ? V3D_3D_TIMEOUT : V3D_3D_MMU_FAULT;
    }
    return V3D_3D_DONE;
}

/* Geometry compute route (geom_raster_cs.h), TWO-PASS: vertex shading is
 * hoisted out of the rasterizer so each vertex is transformed+lit exactly once.
 * Both passes are CSD dispatches under ONE op bracket (the dispatch lock is
 * owner-recursive) so no other worker can interleave and clobber the shared
 * `vout` intermediate between them.
 *
 *   Pass 1 - cs2 (xform): grid ceil(nverts/lane) workgroups, one invocation per
 *     vertex.  SSBO 0 = vout (8-float/32-byte records), 1 = positions, 2 =
 *     normals (gouraud) / instance matrix (gears).  dst = vout so its POST
 *     writeback cleans the records to DRAM for pass 2 to read.
 *   Pass 2 - cs (raster): ceil(ntris/tpw) workgroups.  SSBO 0 = fb, 1 = vout,
 *     2 = depth (gouraud) / 3 = texture (textured).  dst = fb + aux = depth for
 *     the POST writeback (ranged over both surfaces).
 *
 * The frame's first draw also clears colour + depth on the GPU (two write-only
 * fills), replacing the client's per-frame CPU clear of both canvases.
 *
 * The per-draw uniform blob is the concatenation [cs2/xform | cs/raster]; the
 * split is the program's cs2_uniform_blob_dwords. */
static int draw_compute_geom(const v3d_3d_prog_t *pr, uint32_t fb_iova,
                             uint32_t stride_bytes, uint32_t fmt,
                             uint32_t w, uint32_t h, uint32_t nverts,
                             uint32_t pos_iova, uint32_t nrm_iova,
                             uint32_t depth_iova, uint32_t z_stride,
                             const uint32_t *unif, uint32_t num_uniforms,
                             uint32_t tex_iova, uint32_t tex_span,
                             uint32_t inst_iova, uint32_t inst_span, int is_inst,
                             int is_texcol, int first_frame, uint32_t clear_rgba)
{
    v3d_g2d_csd_job_t job;
    uint32_t ssbo[4];
    uint32_t ntris = nverts / 3u;
    uint32_t split = pr->cs2_uniform_blob_dwords;
    uint32_t xwg;
    int rc;

    if (fmt != V3D_3D_FMT_RGBA8)
        return decline("geom: fmt not RGBA8");
    if ((stride_bytes & 3u) != 0 || stride_bytes < w * 4u)
        return decline("geom: stride");
    if ((z_stride & 3u) != 0 || z_stride < w * 4u)
        return decline("geom: depth stride");
    if (ntris == 0 || ntris >= (1u << 16))
        return decline("geom: triangle count");
    if (nverts > V3D_3D_MAX_VERTS)
        return decline("geom: nverts > vout");
    if (pr->cs_local_size[0] == 0 || pr->cs_local_size[1] == 0 ||
        pr->cs_local_size[2] != 1u)
        return decline("geom: local_size");
    if (pr->cs_shared_size != 0)
        return decline("geom: shared memory unsupported");
    /* pass 1 (cs2/xform) is mandatory on the two-pass geom route */
    if (pr->cs2_qpu_words == 0 ||
        pr->cs2_local_size[0] == 0 || pr->cs2_local_size[1] == 0 ||
        pr->cs2_local_size[2] != 1u)
        return decline("geom: xform stage missing");
    if (pr->cs2_shared_size != 0)
        return decline("geom: xform shared memory unsupported");
    if (split > num_uniforms)
        return decline("geom: uniform blob shorter than xform region");
    /* The daemon-side frame clear fills tightly-packed rows only; a pitched
     * colour or depth canvas would be left half-cleared, so decline it and
     * the client keeps its own CPU clear (pgl_g3d always sends w*4). */
    if (first_frame && (stride_bytes != w * 4u || z_stride != w * 4u))
        return decline("geom: first_frame pitched strides");

    v3d_g2d_op_begin();

    /* Frame clear on the GPU, before either pass.  The fills are write-only;
     * pass 1's POST drains the write combiner before pass 2's depth atomics
     * read, and pass 2's POST writes fb+depth back to DRAM.  The geom fb layout
     * is 0xAARRGGBB while clear_rgba is the back end's 0xAABBGGRR, so R and B
     * swap. */
    if (first_frame) {
        uint32_t c = clear_rgba;
        uint32_t px = (c & 0xFF000000u) | ((c & 0x000000FFu) << 16) |
                      (c & 0x0000FF00u) | ((c & 0x00FF0000u) >> 16);

        if (!gpu_fill(fb_iova, NULL, (int32_t)w, (int32_t)h,
                      0, 0, (int32_t)w, (int32_t)h, px) ||
            !gpu_fill(depth_iova, NULL, (int32_t)w, (int32_t)h,
                      0, 0, (int32_t)w, (int32_t)h, 0xFFFFFFFFu)) {
            v3d_g2d_op_end();
            return decline("geom: frame clear fill");
        }
    }

    /* ---- Pass 1: vertex transform -> vout ---------------------------- */
    ssbo[0] = _vout_p;
    ssbo[1] = pos_iova;
    if (tex_iova != 0u) {
        /* textured xform reads interleaved pos+uv from binding 1 only */
        if (expand_uniforms(_ccu_unif_va, pr->cs2_unif_contents, pr->cs2_unif_data,
                            pr->cs2_num_uniforms, unif, num_uniforms, NULL,
                            0.0f, 0.0f, 0.0f, 0.0f, ssbo, 2u) != 0) {
            v3d_g2d_op_end();
            return decline("geom: xform uniforms (tex)");
        }
    } else if (is_inst) {
        /* instanced xform: binding 2 = normals, binding 3 = the per-instance
         * matrix SSBO (gears) the client refills every frame. */
        ssbo[2] = nrm_iova;
        ssbo[3] = inst_iova;
        if (expand_uniforms(_ccu_unif_va, pr->cs2_unif_contents, pr->cs2_unif_data,
                            pr->cs2_num_uniforms, unif, num_uniforms, NULL,
                            0.0f, 0.0f, 0.0f, 0.0f, ssbo, 4u) != 0) {
            v3d_g2d_op_end();
            return decline("geom: xform uniforms (inst)");
        }
    } else {
        ssbo[2] = nrm_iova;
        if (expand_uniforms(_ccu_unif_va, pr->cs2_unif_contents, pr->cs2_unif_data,
                            pr->cs2_num_uniforms, unif, num_uniforms, NULL,
                            0.0f, 0.0f, 0.0f, 0.0f, ssbo, 3u) != 0) {
            v3d_g2d_op_end();
            return decline("geom: xform uniforms");
        }
    }
    __asm__ __volatile__("dsb sy" ::: "memory");

    xwg = pr->cs2_local_size[0] * pr->cs2_local_size[1];
    memset(&job, 0, sizeof(job));
    job.code_iova   = pr->cs2_qpu_iova;
    job.unif_iova   = _ccu_unif_p;
    job.wg_count[0] = (nverts + xwg - 1u) / xwg;   /* one invocation per vertex */
    job.wg_count[1] = 1u;
    job.wg_count[2] = 1u;
    job.wg_size     = xwg;
    job.threads     = pr->cs2_threads;
    job.single_seg  = pr->cs2_single_seg;
    job.dst_phy     = _vout_p;          /* POST writeback: vout -> DRAM */
    job.dst_span    = nverts * (is_texcol ? 48u : 32u);
    job.unif_span   = pr->cs2_num_uniforms * 4u;
    job.tag         = V3D_G2D_TAG_GEOM_XFORM;
    /* Ranged PRE extent: the only ARM-written surfaces this pass reads are the
     * vertex streams, so the hull of the present streams bounds run_csd's
     * pre-job L2T walk (the uniform block is walked separately). */
    {
        uint32_t lo = pos_iova;
        uint32_t hi = pos_iova + nverts * (is_texcol ? 40u :
                                             (tex_iova != 0u ? 24u :
                                              (is_inst ? 16u : 12u)));

        if (tex_iova == 0u && nrm_iova != 0u) {
            uint32_t nlo = nrm_iova;
            uint32_t nhi = nrm_iova + nverts * 12u;

            if (nlo < lo) lo = nlo;
            if (nhi > hi) hi = nhi;
        }
        if (is_inst && inst_iova != 0u) {
            uint32_t ghi = inst_iova + inst_span;

            if (inst_iova < lo) lo = inst_iova;
            if (ghi > hi) hi = ghi;
        }
        job.src_phy  = lo;
        job.src_span = hi - lo;
    }
    job.budget_ms   = V3D_3D_COMPUTE_BUDGET_MS;
    rc = v3d_g2d_run_csd(&job, V3D_G2D_MAINT_ALL | V3D_G2D_POLL_YIELD);
    if (rc != 0) {
        v3d_g2d_op_end();
        slog("v3d_3d: geom xform rc=%d nverts %u wg=%u thr=%u unif=%u\r\n",
             rc, nverts, job.wg_size, (uint32_t)pr->cs2_threads,
             pr->cs2_num_uniforms);
        return rc > 0 ? V3D_3D_TIMEOUT : V3D_3D_MMU_FAULT;
    }

    /* ---- Pass 2: rasterize from vout --------------------------------- */
    ssbo[0] = fb_iova;
    ssbo[1] = _vout_p;
    ssbo[2] = depth_iova;
    if (tex_iova != 0u)
        ssbo[3] = tex_iova;
    /* the raster kernel's uniforms live in the SECOND region of the blob */
    if (expand_uniforms(_ccu_unif_va, pr->cs_unif_contents, pr->cs_unif_data,
                        pr->cs_num_uniforms, unif + split, num_uniforms - split,
                        NULL, 0.0f, 0.0f, 0.0f, 0.0f,
                        ssbo, tex_iova != 0u ? 4u : 3u) != 0) {
        v3d_g2d_op_end();
        return decline("geom: raster uniforms");
    }
    __asm__ __volatile__("dsb sy" ::: "memory");

    memset(&job, 0, sizeof(job));
    job.code_iova   = pr->cs_qpu_iova;
    job.unif_iova   = _ccu_unif_p;
    /* Raster grid: the legacy kernels map one workgroup to one triangle
     * (cs_geom_tpw 0/1); the one-lane-per-triangle kernels cover cs_geom_tpw
     * triangles per workgroup, so the grid is ceil(ntris/tpw). */
    {
        uint32_t tpw = pr->cs_geom_tpw;
        if (tpw == 0u)
            tpw = 1u;
        job.wg_count[0] = (ntris + tpw - 1u) / tpw;
    }
    job.wg_count[1] = 1u;
    job.wg_count[2] = 1u;
    job.wg_size     = pr->cs_local_size[0] * pr->cs_local_size[1];
    job.threads     = pr->cs_threads;
    job.single_seg  = pr->cs_single_seg;
    job.dst_phy     = fb_iova;
    job.dst_span    = stride_bytes * h;
    job.aux_phy     = depth_iova;   /* atomicMin depth canvas writeback */
    job.aux_span    = z_stride * h;
    job.unif_span   = pr->cs_num_uniforms * 4u;
    job.tag         = V3D_G2D_TAG_GEOM_RASTER;
    if (tex_iova != 0u) {
        job.src_phy  = tex_iova;    /* ARM-written at upload: ranged PRE */
        job.src_span = tex_span;
    }
    job.budget_ms   = V3D_3D_COMPUTE_BUDGET_MS;
    rc = v3d_g2d_run_csd(&job, (tex_iova != 0u ? V3D_G2D_MAINT_PRE : 0u) |
                               V3D_G2D_MAINT_POST | V3D_G2D_POLL_YIELD);
    v3d_g2d_op_end();
    if (rc != 0) {
        slog("v3d_3d: geom raster rc=%d tris %u wg=%u thr=%u unif=%u\r\n",
             rc, ntris, job.wg_size,
             (uint32_t)pr->cs_threads, pr->cs_num_uniforms);
        return rc > 0 ? V3D_3D_TIMEOUT : V3D_3D_MMU_FAULT;
    }
    return V3D_3D_DONE;
}

/* Full-frame colour clear through the 2D gpu_fill entry: V3D 4.2 routes a
 * full-surface fill to the TLB render clear, Pi3/VC4 to the span filler, Pi5 to
 * the CSD fill kernel.  The V3D binner/render RCL clear is NOT ported to raspix,
 * but this clear is NOT optional - the client's pgl_g3d_init issues it as a
 * mandatory bring-up probe and, if it declines, latches the whole GPU path off
 * (_pgl_gpu_off, sticky) so every later draw silently falls back to the CPU.
 * Declining here was why gears/teapot reported CPU on a healthy Pi4: the probe
 * clear returned G2D_ERR_NOT_SUPPORTED and pgl_g3d_init aborted.  gpu_fill
 * writes tightly-packed 32-bit pixels, so a pitched stride or a non-RGBA8
 * format still declines (the client always sends w*4 / RGBA8).  The client
 * colour is 0xAABBGGRR while the framebuffer is 0xAARRGGBB, so R and B swap
 * exactly as the geometry rasterizer's first_frame clear does. */
int v3d_3d_clear(uint32_t fb_iova, uint32_t stride_bytes, uint32_t fmt,
                 uint32_t w, uint32_t h, uint32_t rgba8, int rb_swap)
{
    uint32_t px;
    int gr;

    if (!_ready)
        return V3D_3D_UNSUPPORTED;
    if (w == 0 || h == 0 || fb_iova == 0)
        return V3D_3D_UNSUPPORTED;
    if (fmt != V3D_3D_FMT_RGBA8 || stride_bytes != w * 4u)
        return V3D_3D_UNSUPPORTED;
    if (!v3d_g2d_phy_valid(fb_iova, (size_t)stride_bytes * h))
        return V3D_3D_UNSUPPORTED;

    px = rb_swap ? rgba8 :
         ((rgba8 & 0xFF000000u) | ((rgba8 & 0x000000FFu) << 16) |
          (rgba8 & 0x0000FF00u) | ((rgba8 & 0x00FF0000u) >> 16));

    gr = gpu_fill(fb_iova, NULL, (int32_t)w, (int32_t)h,
                  0, 0, (int32_t)w, (int32_t)h, px);
    if (gr == GPU_DONE)
        return V3D_3D_DONE;
    if (gr == GPU_FAILED)
        return V3D_3D_FAILED;
    return V3D_3D_UNSUPPORTED;
}

int v3d_3d_draw(uint32_t fb_iova, uint32_t stride_bytes, uint32_t fmt,
                uint32_t w, uint32_t h, int mode, uint32_t nverts,
                uint32_t pos_iova, uint32_t col_iova,
                uint32_t rgba, int rb_swap,
                uint32_t depth_func, uint32_t depth_mask, uint32_t cull_en,
                uint32_t z_iova, uint32_t z_stride, int first_frame,
                uint32_t clear_rgba, float clear_z,
                const v3d_3d_ext_t *ext,
                const v3d_3d_tex_t *tex,
                int32_t prog_handle, const uint32_t *unif,
                uint32_t num_uniforms,
                const v3d_3d_ctex_t *ctex)
{
    int compiled = (prog_handle >= 0);

    (void)rgba; (void)rb_swap; (void)depth_func; (void)depth_mask;
    (void)cull_en; (void)clear_z; (void)ext; (void)tex;

    /* Compute route: a program carrying a compute stage dispatches through the
     * CSD; this is the ONLY GPU raster path on raspix. */
    if (compiled && _ready && (uint32_t)prog_handle < V3D_3D_MAX_PROGS &&
        _progs[prog_handle].used && _progs[prog_handle].cs_qpu_words != 0) {
        if (w == 0 || h == 0 || fb_iova == 0)
            return decline("compute: zero w/h/fb");
        if (!v3d_g2d_phy_valid(fb_iova, (size_t)stride_bytes * h))
            return decline("compute: fb phys");
        if (_progs[prog_handle].cs_is_geom) {
            /* geometry rasterizer: triangles only; the staged streams and the
             * depth canvas ride the draw's own addresses.  cs_is_geom == 2 is
             * the TEXTURED kernel (pos carries interleaved pos+uv, the texture
             * image arrives as ctex unit 0); cs_is_geom == 3 is the INSTANCED
             * kernel (gears: pos carries vec4 pos+instance-index, col the
             * float3 normals, ctex unit 0 the per-frame instance-matrix SSBO);
             * cs_is_geom == 4 is the COLOUR-MODULATED TEXTURED kernel (imgui):
             * pos carries interleaved pos+uv+rgba (10-float stride, nverts*40
             * bytes), col is unused (zero canvas), ctex unit 0 the linear
             * font-atlas image; the raster kernel alpha-blends and clips
             * (uScissor) with no depth write, and its vout record is 12 floats.
             * The texcol route shares the textured two-pass raster, so it runs
             * on V3D 4.2 (Pi4) exactly as on 7.1 (Pi5); Pi3 declines at
             * v3d_3d_init (no CSD compute engine). */
            int gtex = (_progs[prog_handle].cs_is_geom == 2);
            int ginst = (_progs[prog_handle].cs_is_geom == 3);
            int gtc = (_progs[prog_handle].cs_is_geom == 4);
            /* textured family (plain tex + colour-modulated tex): col unused,
             * ctex[0] is the linear image the raster kernel indexes directly. */
            int gtexc = gtex || gtc;
            uint32_t pstride = gtc ? 40u : (gtex ? 24u : (ginst ? 16u : 12u));
            if (mode != V3D_3D_MODE_TRIANGLES)
                return decline("geom: mode not triangles");
            if (nverts == 0 || (nverts % 3u) != 0)
                return decline("geom: nverts not multiple of 3");
            if (!v3d_g2d_phy_valid(pos_iova, (size_t)nverts * pstride))
                return decline("geom: pos phys");
            if (!gtexc && !v3d_g2d_phy_valid(col_iova, (size_t)nverts * 12u))
                return decline("geom: nrm phys");
            if (gtexc && (ctex == NULL || ctex->ctex_iova[0] == 0))
                return decline("geom tex: no texture bound");
            if (gtexc && (ctex->ctex_stride[0] < ctex->ctex_w[0] * 4u ||
                !v3d_g2d_phy_valid(ctex->ctex_iova[0],
                                   (size_t)ctex->ctex_stride[0] * ctex->ctex_h[0])))
                return decline("geom tex: tex phys");
            if (ginst && (ctex == NULL || ctex->ctex_iova[0] == 0))
                return decline("geom inst: no instance buffer bound");
            if (ginst && (ctex->ctex_stride[0] < ctex->ctex_w[0] * 4u ||
                !v3d_g2d_phy_valid(ctex->ctex_iova[0],
                                   (size_t)ctex->ctex_stride[0] * ctex->ctex_h[0])))
                return decline("geom inst: instance phys");
            if (z_iova == 0 ||
                !v3d_g2d_phy_valid(z_iova, (size_t)z_stride * h))
                return decline("geom: depth phys");
            return draw_compute_geom(&_progs[prog_handle], fb_iova,
                                     stride_bytes, fmt, w, h, nverts,
                                     pos_iova, col_iova, z_iova, z_stride,
                                     unif, num_uniforms,
                                     gtexc ? ctex->ctex_iova[0] : 0u,
                                     gtexc ? ctex->ctex_stride[0] *
                                             ctex->ctex_h[0] : 0u,
                                     ginst ? ctex->ctex_iova[0] : 0u,
                                     ginst ? ctex->ctex_stride[0] *
                                             ctex->ctex_h[0] : 0u,
                                     ginst, gtc, first_frame, clear_rgba);
        }
        return draw_compute(&_progs[prog_handle], fb_iova, stride_bytes, fmt,
                            w, h, unif, num_uniforms, ctex);
    }

    /* The V3D binner/render pipeline and the fixed-function shaders are NOT
     * ported to raspix: only the CSD compute route runs on the GPU.  Every
     * other draw declines so the client keeps its CPU/NEON rasterizer. */
    (void)pos_iova; (void)col_iova; (void)z_iova; (void)z_stride;
    (void)first_frame; (void)clear_rgba; (void)unif; (void)num_uniforms;
    (void)ctex; (void)nverts; (void)mode; (void)fmt; (void)stride_bytes;
    (void)w; (void)h; (void)fb_iova;
    return compiled ? decline("raspix: non-compute draw") : V3D_3D_UNSUPPORTED;
}

/* ------------------------------------------------------------------ */
/* Compiled-program upload (WS4): copy the VS/FS/coord/cs/cs2 QPU words into  */
/* GPU dma via alloc_stage and cache the addresses + prog_data by handle.  No */
/* dispatch happens here; a later draw references the returned handle.        */
/* ------------------------------------------------------------------ */
int v3d_3d_program(const v3d_3d_prog_desc_t *desc)
{
    v3d_3d_prog_t *pr;
    void *va;
    uint32_t iova, i;
    int slot = -1;
    /* A FULL-SCREEN client program (shadertoy-style) arrives as a PURE-COMPUTE
     * blob: the pgl_g3d gate compiles only the compute wrap and deliberately
     * keeps an empty VS/FS/COORD triple.  raspix only ever runs the CSD compute
     * route, so the raster stages are optional whenever a cs kernel is present. */
    int cs_only = (desc != NULL && desc->cs_qpu_words != 0 &&
                   desc->vs_qpu_words == 0 && desc->fs_qpu_words == 0 &&
                   desc->coord_qpu_words == 0);

    if (!_ready || desc == NULL)
        return V3D_3D_PROG_UNSUPPORTED;

    /* validate the stage payloads against the cache caps (decline, never
     * truncate, so an out-of-range program falls back to the CPU path).
     * Emptiness alone is NOT a cap violation: a pure-compute program is
     * legitimately empty in vs/fs/coord and is checked by cs_only below. */
    if (desc->vs_qpu_words > V3D_3D_PROG_MAX_QPU ||
        desc->fs_qpu_words > V3D_3D_PROG_MAX_QPU ||
        desc->coord_qpu_words > V3D_3D_PROG_MAX_QPU ||
        desc->cs_qpu_words > V3D_3D_PROG_MAX_QPU ||
        desc->vs_num_uniforms > V3D_3D_PROG_MAX_UNIF ||
        desc->fs_num_uniforms > V3D_3D_PROG_MAX_UNIF ||
        desc->coord_num_uniforms > V3D_3D_PROG_MAX_UNIF ||
        desc->cs_num_uniforms > V3D_3D_PROG_MAX_UNIF ||
        desc->fs_num_inputs > V3D_3D_PROG_MAX_INPUTS ||
        (!cs_only && (desc->vs_qpu_words == 0 || desc->fs_qpu_words == 0 ||
                      desc->coord_qpu_words == 0))) {
        slog("v3d_3d: program rejected: qpu vs/fs/coord/cs %u/%u/%u/%u (max %u) "
             "unif %u/%u/%u/%u (max %u) inputs %u (max %u)\r\n",
             desc->vs_qpu_words, desc->fs_qpu_words, desc->coord_qpu_words,
             desc->cs_qpu_words, V3D_3D_PROG_MAX_QPU,
             desc->vs_num_uniforms, desc->fs_num_uniforms,
             desc->coord_num_uniforms, desc->cs_num_uniforms,
             V3D_3D_PROG_MAX_UNIF, desc->fs_num_inputs, V3D_3D_PROG_MAX_INPUTS);
        return V3D_3D_PROG_UNSUPPORTED;
    }
    /* an all-empty program (no raster stages, no compute) can never draw */
    if (desc->vs_qpu_words == 0 && desc->fs_qpu_words == 0 &&
        desc->coord_qpu_words == 0 && desc->cs_qpu_words == 0)
        return V3D_3D_PROG_UNSUPPORTED;
    if (!cs_only) {
        if (desc->vs_qpu == NULL || desc->vs_qpu_words == 0 ||
            desc->vs_qpu_words > V3D_3D_PROG_MAX_QPU)
            return V3D_3D_PROG_UNSUPPORTED;
        if (desc->fs_qpu == NULL || desc->fs_qpu_words == 0 ||
            desc->fs_qpu_words > V3D_3D_PROG_MAX_QPU)
            return V3D_3D_PROG_UNSUPPORTED;
        /* the coordinate (binning) shader is mandatory in the shared client
         * blob's raster shape: WS1 always emits it, so a raster blob without
         * one is malformed/stale.  Pure-compute programs never bin. */
        if (desc->coord_qpu == NULL || desc->coord_qpu_words == 0 ||
            desc->coord_qpu_words > V3D_3D_PROG_MAX_QPU)
            return V3D_3D_PROG_UNSUPPORTED;
    }
    if (desc->vs_num_uniforms > V3D_3D_PROG_MAX_UNIF ||
        desc->fs_num_uniforms > V3D_3D_PROG_MAX_UNIF ||
        desc->coord_num_uniforms > V3D_3D_PROG_MAX_UNIF)
        return V3D_3D_PROG_UNSUPPORTED;
    if (desc->fs_num_inputs > V3D_3D_PROG_MAX_INPUTS)
        return V3D_3D_PROG_UNSUPPORTED;
    /* a non-empty uniform stream must carry both its descriptor and values */
    if (desc->vs_num_uniforms &&
        (desc->vs_unif_contents == NULL || desc->vs_unif_data == NULL))
        return V3D_3D_PROG_UNSUPPORTED;
    if (desc->fs_num_uniforms &&
        (desc->fs_unif_contents == NULL || desc->fs_unif_data == NULL))
        return V3D_3D_PROG_UNSUPPORTED;
    if (desc->coord_num_uniforms &&
        (desc->coord_unif_contents == NULL || desc->coord_unif_data == NULL))
        return V3D_3D_PROG_UNSUPPORTED;
    /* optional compute stage */
    if (desc->cs_qpu_words != 0 &&
        (desc->cs_qpu == NULL || desc->cs_qpu_words > V3D_3D_PROG_MAX_QPU ||
         desc->cs_num_uniforms > V3D_3D_PROG_MAX_UNIF ||
         (desc->cs_num_uniforms &&
          (desc->cs_unif_contents == NULL || desc->cs_unif_data == NULL))))
        return V3D_3D_PROG_UNSUPPORTED;
    /* optional second compute stage (pass-1 vertex transform) */
    if (desc->cs2_qpu_words != 0 &&
        (desc->cs2_qpu == NULL || desc->cs2_qpu_words > V3D_3D_PROG_MAX_QPU ||
         desc->cs2_num_uniforms > V3D_3D_PROG_MAX_UNIF ||
         (desc->cs2_num_uniforms &&
          (desc->cs2_unif_contents == NULL || desc->cs2_unif_data == NULL))))
        return V3D_3D_PROG_UNSUPPORTED;
    /* a two-pass geom program must carry BOTH stages */
    if (desc->cs_is_geom && (desc->cs_qpu_words == 0 || desc->cs2_qpu_words == 0))
        return V3D_3D_PROG_UNSUPPORTED;
    /* cs_geom_tpw invariant: a nonzero tpw must equal the cs workgroup lane
     * count (the one-lane-per-triangle kernels cover exactly one triangle per
     * lane); 0 keeps the legacy one-workgroup-per-triangle grid. */
    if (desc->cs_geom_tpw != 0u &&
        desc->cs_geom_tpw != desc->cs_local_size[0] * desc->cs_local_size[1] *
                             desc->cs_local_size[2])
        return V3D_3D_PROG_UNSUPPORTED;

    /* reserve a free cache slot */
    for (i = 0; i < V3D_3D_MAX_PROGS; i++) {
        if (!_progs[i].used) { slot = (int)i; break; }
    }
    if (slot < 0)
        return V3D_3D_PROG_UNSUPPORTED;      /* cache full */
    pr = &_progs[slot];
    memset(pr, 0, sizeof(*pr));

    /* upload the raster-stage QPU code into GPU dma.  A pure-compute program
     * has no raster stages; its slot fields stay zeroed and the compute route
     * never reads them. */
    if (!cs_only) {
        if (alloc_stage_raw(desc->vs_qpu_words * 4u, &va, &iova, &pr->vs_qpu_raw) != 0) {
            slog("v3d_3d: program vs alloc_stage %u failed\r\n", desc->vs_qpu_words * 4u);
            goto fail;
        }
        memcpy(va, desc->vs_qpu, desc->vs_qpu_words * 4u);
        pr->vs_qpu_iova  = iova;
        pr->vs_qpu_words = desc->vs_qpu_words;

        /* upload the FS QPU code */
        if (alloc_stage_raw(desc->fs_qpu_words * 4u, &va, &iova, &pr->fs_qpu_raw) != 0)
            goto fail;
        memcpy(va, desc->fs_qpu, desc->fs_qpu_words * 4u);
        pr->fs_qpu_iova  = iova;
        pr->fs_qpu_words = desc->fs_qpu_words;

        /* upload the COORD (binning) QPU code */
        if (alloc_stage_raw(desc->coord_qpu_words * 4u, &va, &iova, &pr->coord_qpu_raw) != 0)
            goto fail;
        memcpy(va, desc->coord_qpu, desc->coord_qpu_words * 4u);
        pr->coord_qpu_iova  = iova;
        pr->coord_qpu_words = desc->coord_qpu_words;
    }

    /* upload the optional COMPUTE QPU code */
    if (desc->cs_qpu_words != 0) {
        if (alloc_stage_raw(desc->cs_qpu_words * 4u, &va, &iova, &pr->cs_qpu_raw) != 0)
            goto fail;
        memcpy(va, desc->cs_qpu, desc->cs_qpu_words * 4u);
        pr->cs_qpu_iova  = iova;
        pr->cs_qpu_words = desc->cs_qpu_words;
        if (desc->cs_num_uniforms) {
            memcpy(pr->cs_unif_contents, desc->cs_unif_contents,
                   desc->cs_num_uniforms * 4u);
            memcpy(pr->cs_unif_data, desc->cs_unif_data,
                   desc->cs_num_uniforms * 4u);
        }
        pr->cs_num_uniforms = desc->cs_num_uniforms;
        pr->cs_threads      = desc->cs_threads;
        pr->cs_single_seg   = desc->cs_single_seg;
        pr->cs_local_size[0] = desc->cs_local_size[0];
        pr->cs_local_size[1] = desc->cs_local_size[1];
        pr->cs_local_size[2] = desc->cs_local_size[2];
        pr->cs_shared_size  = desc->cs_shared_size;
        pr->cs_is_geom      = desc->cs_is_geom;
        pr->cs_geom_tpw     = desc->cs_geom_tpw;
    }

    /* upload the optional second COMPUTE QPU (pass-1 vertex transform) */
    if (desc->cs2_qpu_words != 0) {
        if (alloc_stage_raw(desc->cs2_qpu_words * 4u, &va, &iova, &pr->cs2_qpu_raw) != 0)
            goto fail;
        memcpy(va, desc->cs2_qpu, desc->cs2_qpu_words * 4u);
        pr->cs2_qpu_iova  = iova;
        pr->cs2_qpu_words = desc->cs2_qpu_words;
        if (desc->cs2_num_uniforms) {
            memcpy(pr->cs2_unif_contents, desc->cs2_unif_contents,
                   desc->cs2_num_uniforms * 4u);
            memcpy(pr->cs2_unif_data, desc->cs2_unif_data,
                   desc->cs2_num_uniforms * 4u);
        }
        pr->cs2_num_uniforms = desc->cs2_num_uniforms;
        pr->cs2_threads      = desc->cs2_threads;
        pr->cs2_single_seg   = desc->cs2_single_seg;
        pr->cs2_local_size[0] = desc->cs2_local_size[0];
        pr->cs2_local_size[1] = desc->cs2_local_size[1];
        pr->cs2_local_size[2] = desc->cs2_local_size[2];
        pr->cs2_shared_size  = desc->cs2_shared_size;
        pr->cs2_uniform_blob_dwords = desc->cs2_uniform_blob_dwords;
    }

    /* cache the uniform layout descriptors AND their per-slot args on the CPU */
    if (desc->vs_num_uniforms) {
        memcpy(pr->vs_unif_contents, desc->vs_unif_contents,
               desc->vs_num_uniforms * 4u);
        memcpy(pr->vs_unif_data, desc->vs_unif_data,
               desc->vs_num_uniforms * 4u);
    }
    pr->vs_num_uniforms = desc->vs_num_uniforms;

    if (desc->fs_num_uniforms) {
        memcpy(pr->fs_unif_contents, desc->fs_unif_contents,
               desc->fs_num_uniforms * 4u);
        memcpy(pr->fs_unif_data, desc->fs_unif_data,
               desc->fs_num_uniforms * 4u);
    }
    pr->fs_num_uniforms = desc->fs_num_uniforms;

    if (desc->coord_num_uniforms) {
        memcpy(pr->coord_unif_contents, desc->coord_unif_contents,
               desc->coord_num_uniforms * 4u);
        memcpy(pr->coord_unif_data, desc->coord_unif_data,
               desc->coord_num_uniforms * 4u);
    }
    pr->coord_num_uniforms = desc->coord_num_uniforms;

    /* cache the vertex-stage prog_data */
    pr->vs_threads           = desc->vs_threads;
    pr->vs_single_seg        = desc->vs_single_seg;
    pr->vs_vpm_input_size    = desc->vs_vpm_input_size;
    pr->vs_vpm_output_size   = desc->vs_vpm_output_size;
    pr->vs_vcm_cache_size    = desc->vs_vcm_cache_size;
    pr->vs_separate_segments = desc->vs_separate_segments;
    memcpy(pr->vs_vattr_sizes, desc->vs_vattr_sizes,
           sizeof(pr->vs_vattr_sizes));
    memcpy(pr->vs_driver_location_map, desc->vs_driver_location_map,
           sizeof(pr->vs_driver_location_map));

    /* cache the fragment-stage prog_data */
    pr->fs_threads       = desc->fs_threads;
    pr->fs_single_seg    = desc->fs_single_seg;
    pr->fs_needs_coords  = desc->fs_needs_coords;
    pr->fs_uses_center_w = desc->fs_uses_center_w;
    pr->fs_writes_z      = desc->fs_writes_z;
    pr->fs_disable_ez    = desc->fs_disable_ez;
    pr->fs_num_inputs    = desc->fs_num_inputs;
    memcpy(pr->fs_input_slots, desc->fs_input_slots,
           sizeof(pr->fs_input_slots));

    /* cache the coordinate-stage prog_data */
    pr->coord_threads           = desc->coord_threads;
    pr->coord_single_seg        = desc->coord_single_seg;
    pr->coord_vpm_input_size    = desc->coord_vpm_input_size;
    pr->coord_vpm_output_size   = desc->coord_vpm_output_size;
    pr->coord_vcm_cache_size    = desc->coord_vcm_cache_size;
    pr->coord_separate_segments = desc->coord_separate_segments;

    __asm__ __volatile__("dsb sy" ::: "memory");
    /* The per-dispatch PRE is bounded to the uniform block and the job's
     * source extent; the code blocks are not in it.  Drop whatever the
     * caches still hold for these dma addresses (a freed program's
     * instructions) once, here, so every later dispatch fetches this code. */
    v3d_g2d_l2_invalidate_all();
    pr->used = 1;
    return slot;

fail:
    /* release the slot and the dma blocks already uploaded for it */
    slog("v3d_3d: program %d alloc_stage failed (fs %u coord %u cs %u words)\r\n",
         slot, desc->fs_qpu_words, desc->coord_qpu_words, desc->cs_qpu_words);
    release_prog(pr);
    return -31;
}

/* Drop a cached program: give its QPU code blocks back to dma and free the
 * slot.  A later draw naming the handle is declined (used == 0). */
int v3d_3d_program_free(int prog_handle)
{
    v3d_3d_prog_t *pr;

    if (!_ready || prog_handle < 0 || (uint32_t)prog_handle >= V3D_3D_MAX_PROGS)
        return V3D_3D_PROG_UNSUPPORTED;
    pr = &_progs[prog_handle];
    if (!pr->used)
        return V3D_3D_PROG_UNSUPPORTED;
    pr->used = 0;
    __asm__ __volatile__("dsb sy" ::: "memory");
    release_prog(pr);
    return 0;
}
