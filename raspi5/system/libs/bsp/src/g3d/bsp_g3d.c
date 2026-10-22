/*
 * bsp_g3d.c - raspberry-pi5 portablegl 3D public API: the V3D 7.1
 * 3D-pipeline offload split out of bsp_g2d.  Every operation is delegated
 * to the VideoCore back end in libvideocore (<videocore/vc_pgl3d.h>),
 * which owns the CLE binning + render submission and works zero-copy on
 * the caller's contig-shm / dma buffers through their resolved physical
 * bases.
 *
 * The 3D pipeline is brought up lazily on the first op (idempotent, on top
 * of the CSD path's V3D init that bsp_g2d_init already ran) and latched:
 * once vc_pgl3d_init() declines, we never retry, so a portablegl client
 * sees one sticky G2D_ERR_NOT_SUPPORTED and stays on its own CPU/NEON
 * rasterizer.  These flags are only touched from g2dd's serialized
 * dev_cntl path for a given requester; the vc_pgl3d layer owns the real
 * GPU-dispatch lock.  A submitted dispatch is never replayed.
 */

#include <string.h>

#include <bsp/bsp_g3d.h>
#include <g2dclient/g2dclient.h>

#include <videocore/vc_pgl3d.h>
#include <ewoksys/klog.h>

static int _pgl3d_ready;
static int _pgl3d_declined;

static int pgl3d_ensure(void)
{
    if (_pgl3d_ready)
        return 1;
    if (_pgl3d_declined)
        return 0;
    if (vc_pgl3d_init() == VC_PGL3D_DONE) {
        _pgl3d_ready = 1;
        return 1;
    }
    _pgl3d_declined = 1;
    return 0;
}

/* Map the vc_pgl3d tri-state onto the g2d result codes g2dd passes back
 * verbatim: DONE -> G2D_OK, FAILED -> G2D_ERR_FAILED (transient), and
 * UNSUPPORTED -> G2D_ERR_NOT_SUPPORTED (sticky, the client falls back). */
static int32_t pgl3d_rc(int r)
{
    switch (r) {
    case VC_PGL3D_DONE:
        return G2D_OK;
    case VC_PGL3D_FAILED:
        return G2D_ERR_FAILED;
    case VC_PGL3D_TIMEOUT:
        return G2D_ERR_TIMEOUT;
    case VC_PGL3D_MMU_FAULT:
        return G2D_ERR_MMU_FAULT;
    default: /* VC_PGL3D_UNSUPPORTED */
        return G2D_ERR_NOT_SUPPORTED;
    }
}

int32_t bsp_g3d_init(void)
{
    return pgl3d_ensure() ? 0 : -1;
}

int32_t bsp_g3d_clear(uint32_t *fb, ewokos_addr_t fb_phy, uint8_t contig,
                      int32_t w, int32_t h, uint32_t stride, uint32_t fmt,
                      uint32_t color, int32_t rb_swap)
{
    /* the GPU clears through the physical base only; fb (the render-target
     * VA) is not touched here. */
    (void)fb;
    if (!pgl3d_ensure())
        return G2D_ERR_NOT_SUPPORTED;
    return pgl3d_rc(vc_pgl3d_clear(fb_phy, contig, stride,
                                   (uint32_t)w, (uint32_t)h, fmt, color,
                                   rb_swap));
}

int32_t bsp_g3d_draw(ewokos_addr_t fb_phy, uint8_t fb_contig,
                     int32_t w, int32_t h, uint32_t stride, uint32_t fmt,
                     uint32_t mode, uint32_t nverts,
                     ewokos_addr_t pos_phy, uint8_t pos_contig,
                     ewokos_addr_t col_phy, uint8_t col_contig,
                     uint32_t fs_id, uint32_t color, int32_t rb_swap,
                     uint32_t depth_func, int32_t depth_mask,
                     int32_t blend_en, int32_t cull_en,
                     ewokos_addr_t z_phy, uint8_t z_contig,
                     uint32_t z_stride, uint8_t first_frame,
                     uint32_t clear_rgba, float clear_z,
                     const void* ext, const void* tex,
                     int32_t prog_handle, const void* unif,
                     uint32_t num_uniforms, const void* ctex)
{
    vc_pgl3d_draw_req_t req;

    if (!pgl3d_ensure())
        return G2D_ERR_NOT_SUPPORTED;

    memset(&req, 0, sizeof(req));
    req.mode       = mode;
    req.nverts     = nverts;
    req.pos_phy    = pos_phy;
    req.pos_contig = pos_contig;
    req.col_phy    = col_phy;
    req.col_contig = col_contig;
    req.fb_phy     = fb_phy;
    req.fb_contig  = fb_contig;
    req.stride     = stride;
    req.w          = (uint32_t)w;
    req.h          = (uint32_t)h;
    req.fmt        = fmt;
    req.fs_id      = fs_id;
    req.rgba       = color;
    req.rb_swap    = rb_swap;
    req.depth_func = depth_func;
    req.depth_mask = (uint8_t)depth_mask;
    req.blend_en   = (uint8_t)blend_en;
    req.cull_en    = (uint8_t)cull_en;
    /* persistent Z32F depth surface + frame boundary (Phase A2): z_phy == 0
     * falls back to tile-local Z; first_frame marks the frame's first
     * dispatch (clears colour to clear_rgba + Z to clear_z), later dispatches
     * LOAD the persisted colour+Z so a multi-draw frame accumulates. */
    req.z_phy       = z_phy;
    req.z_contig    = z_contig;
    req.z_stride    = z_stride;
    req.first_frame = first_frame;
    req.clear_rgba  = clear_rgba;
    req.clear_z     = clear_z;
    /* Phase B2c extended render state (scissor / polygon offset / colour write
     * mask).  ext is an opaque pointer to the caller's g2d_pgl_ext_t (the g2dd
     * handler passes &req.ext); a NULL ext from an older caller leaves every
     * field zero = scissor-off / no-offset / write-all, matching the memset. */
    if (ext) {
        const g2d_pgl_ext_t* e = (const g2d_pgl_ext_t*)ext;
        req.scissor_x   = e->scissor_x;
        req.scissor_y   = e->scissor_y;
        req.scissor_w   = e->scissor_w;
        req.scissor_h   = e->scissor_h;
        req.poly_enable = e->poly_enable;
        req.poly_factor = e->poly_factor;
        req.poly_units  = e->poly_units;
        req.color_mask  = e->color_mask;
    }
    /* Phase D texture state (fs_id == G2D_PGL_FS_TEX_MOD).  tex is an opaque
     * pointer to the caller's g2d_pgl_tex_t (the g2dd handler passes a filled
     * struct when it could resolve tex + uv canvases); a NULL tex from an
     * older caller leaves every field zero = the back end declines TEX_MOD
     * back to UNSUPPORTED, matching the memset. */
    if (tex) {
        const g2d_pgl_tex_t* t = (const g2d_pgl_tex_t*)tex;
        req.tex_phy    = t->tex_phy;
        req.tex_contig = t->tex_contig;
        req.tex_stride = t->tex_stride;
        req.tex_w      = t->tex_w;
        req.tex_h      = t->tex_h;
        req.uv_phy     = t->uv_phy;
        req.uv_contig  = t->uv_contig;
    }
    /* WS4 compiled-shader draw reference.  prog_handle >= 0 selects an
     * on-device-compiled program (uploaded via bsp_g3d_program); `unif` is the
     * CPU-side per-draw GL uniform storage (num_uniforms u32 values) that the
     * back end's QUNIFORM interpreter indexes for QUNIFORM_UNIFORM slots, or
     * NULL when num_uniforms == 0.  prog_handle < 0 keeps the fixed-function
     * fs_id path (every field zero through the memset). */
    req.prog_handle  = prog_handle;
    req.unif         = (const uint32_t*)unif;
    req.num_uniforms = num_uniforms;
    /* WS4 compiled-shader sampler bindings.  ctex is an opaque pointer to the
     * caller's g2d_pgl_ctex_t (the g2dd handler passes a filled struct when
     * the draw carries a compiled prog_handle >= 0 and could resolve the
     * sampler-unit canvases); a NULL ctex from an older caller leaves every
     * ctex_phy[] zero through the memset, i.e. no texture unit bound, which
     * is safe for programs whose FS references no sampler.  Only the raspi5
     * back end interprets this block; the six stubs void-cast the pointer. */
    if (ctex) {
        const g2d_pgl_ctex_t* c = (const g2d_pgl_ctex_t*)ctex;
        memcpy(req.ctex_phy,    c->ctex_phy,    sizeof(req.ctex_phy));
        memcpy(req.ctex_contig, c->ctex_contig, sizeof(req.ctex_contig));
        memcpy(req.ctex_stride, c->ctex_stride, sizeof(req.ctex_stride));
        memcpy(req.ctex_w,      c->ctex_w,      sizeof(req.ctex_w));
        memcpy(req.ctex_h,      c->ctex_h,      sizeof(req.ctex_h));
        memcpy(req.ctex_tiling, c->ctex_tiling, sizeof(req.ctex_tiling));
        memcpy(req.ctex_ub_pad, c->ctex_ub_pad, sizeof(req.ctex_ub_pad));
        memcpy(req.ctex_depth,       c->ctex_depth,       sizeof(req.ctex_depth));
        memcpy(req.ctex_arrstride64, c->ctex_arrstride64, sizeof(req.ctex_arrstride64));
    }
    return pgl3d_rc(vc_pgl3d_draw(&req));
}

/* Upload a compiled shader program.  `blob` is the CPU-side serialized
 * g2d_pgl_program_blob_t (fixed header + the nine payload sections in wire
 * order: VS QPU, FS QPU, COORD QPU, VS uniform contents, VS uniform data,
 * FS uniform contents, FS uniform data, COORD uniform contents, COORD uniform
 * data) that pgl_g3d_shader packed on the client; `size`
 * is the attached canvas byte count.  This is the only layer that both knows
 * the blob layout and owns a 3D back end, so it validates the header + payload
 * bounds here, then points a vc_pgl3d_program_req_t at the sections IN PLACE
 * (zero-copy: vc_pgl3d/v3d_3d deep-copies the QPU + uniform data into GPU dma)
 * and forwards.  Returns the non-negative program handle, or a negative
 * G2D_ERR_* code: FAILED for a malformed/truncated blob (transient),
 * NOT_SUPPORTED when there is no 3D path or the back end declines the program
 * (sticky - the client keeps that program on the CPU).  VC_PGL3D_PROG_* and
 * G2D_ERR_* codes are value-identical, so the back end's result passes through
 * verbatim.  The attribute/input caps agree across the boundary
 * (VC_PGL3D_PROG_MAX_ATTRS == G2D_PGL_MAX_VS_ATTRS, MAX_INPUTS likewise), so
 * the fixed-size array copies below are exact. */
int32_t bsp_g3d_program(const void* blob, uint32_t size)
{
    const g2d_pgl_program_blob_t* h;
    const uint8_t* base = (const uint8_t*)blob;
    vc_pgl3d_program_req_t req;
    uint32_t off;
    uint64_t expect;

    if (!pgl3d_ensure())
        return G2D_ERR_NOT_SUPPORTED;
    if (blob == NULL || size < sizeof(g2d_pgl_program_blob_t)) {
        slog("bsp_g3d: program blob too small %u\n", size);
        return -20;
    }

    h = (const g2d_pgl_program_blob_t*)blob;
    if (h->magic != G2D_PGL_PROGRAM_MAGIC ||
        h->version != G2D_PGL_PROGRAM_VERSION) {
        slog("bsp_g3d: program magic 0x%x version %u (want %u)\n",
             h->magic, h->version, (uint32_t)G2D_PGL_PROGRAM_VERSION);
        return -21;
    }

    /* the declared payload must match the section counts (accumulated in 64
     * bits so a corrupt count cannot wrap past the check) and fit inside the
     * attached canvas; only then are the section pointers below in bounds. */
    expect = ((uint64_t)h->vs_qpu_words + (uint64_t)h->fs_qpu_words +
              (uint64_t)h->coord_qpu_words +
              (uint64_t)h->vs_num_uniforms * 2u +
              (uint64_t)h->fs_num_uniforms * 2u +
              (uint64_t)h->coord_num_uniforms * 2u +
              (uint64_t)h->cs_qpu_words +
              (uint64_t)h->cs_num_uniforms * 2u +
              (uint64_t)h->cs2_qpu_words +
              (uint64_t)h->cs2_num_uniforms * 2u) * 4u;
    if (expect != (uint64_t)h->payload_bytes) {
        slog("bsp_g3d: program payload %u != expect %u\n",
             h->payload_bytes, (uint32_t)expect);
        return -22;
    }
    if ((uint64_t)h->payload_bytes > (uint64_t)size - sizeof(*h)) {
        slog("bsp_g3d: program payload %u > canvas %u\n", h->payload_bytes, size);
        return -23;
    }

    /* walk the twelve payload sections in their fixed wire order */
    off = (uint32_t)sizeof(*h);
    memset(&req, 0, sizeof(req));
    req.vs_qpu = (const uint32_t*)(base + off);
    off += h->vs_qpu_words * 4u;
    req.fs_qpu = (const uint32_t*)(base + off);
    off += h->fs_qpu_words * 4u;
    req.coord_qpu = (const uint32_t*)(base + off);
    off += h->coord_qpu_words * 4u;
    req.vs_unif_contents = (const uint32_t*)(base + off);
    off += h->vs_num_uniforms * 4u;
    req.vs_unif_data = (const uint32_t*)(base + off);
    off += h->vs_num_uniforms * 4u;
    req.fs_unif_contents = (const uint32_t*)(base + off);
    off += h->fs_num_uniforms * 4u;
    req.fs_unif_data = (const uint32_t*)(base + off);
    off += h->fs_num_uniforms * 4u;
    req.coord_unif_contents = (const uint32_t*)(base + off);
    off += h->coord_num_uniforms * 4u;
    req.coord_unif_data = (const uint32_t*)(base + off);
    off += h->coord_num_uniforms * 4u;
    req.cs_qpu = (const uint32_t*)(base + off);
    off += h->cs_qpu_words * 4u;
    req.cs_unif_contents = (const uint32_t*)(base + off);
    off += h->cs_num_uniforms * 4u;
    req.cs_unif_data = (const uint32_t*)(base + off);
    off += h->cs_num_uniforms * 4u;
    /* VERSION 4 second compute stage (cs2 = xform pass): QPU then uniform
     * contents+data, appended after the cs sections. */
    req.cs2_qpu = (const uint32_t*)(base + off);
    off += h->cs2_qpu_words * 4u;
    req.cs2_unif_contents = (const uint32_t*)(base + off);
    off += h->cs2_num_uniforms * 4u;
    req.cs2_unif_data = (const uint32_t*)(base + off);

    req.vs_qpu_words         = h->vs_qpu_words;
    req.vs_num_uniforms      = h->vs_num_uniforms;
    req.vs_threads           = h->vs_threads;
    req.vs_single_seg        = h->vs_single_seg;
    req.vs_vpm_input_size    = h->vs_vpm_input_size;
    req.vs_vpm_output_size   = h->vs_vpm_output_size;
    req.vs_vcm_cache_size    = h->vs_vcm_cache_size;
    req.vs_separate_segments = h->vs_separate_segments;
    memcpy(req.vs_vattr_sizes, h->vs_vattr_sizes, sizeof(req.vs_vattr_sizes));
    memcpy(req.vs_driver_location_map, h->vs_driver_location_map,
           sizeof(req.vs_driver_location_map));

    req.fs_qpu_words         = h->fs_qpu_words;
    req.fs_num_uniforms      = h->fs_num_uniforms;
    req.fs_threads           = h->fs_threads;
    req.fs_single_seg        = h->fs_single_seg;
    req.fs_needs_coords      = h->fs_needs_coords;
    req.fs_uses_center_w     = h->fs_uses_center_w;
    req.fs_writes_z          = h->fs_writes_z;
    req.fs_disable_ez        = h->fs_disable_ez;
    req.fs_num_inputs        = h->fs_num_inputs;
    memcpy(req.fs_input_slots, h->fs_input_slots, sizeof(req.fs_input_slots));

    /* coordinate (binning) stage: the same VS NIR compiled with is_coord=true.
     * Its QPU + uniform stream were walked above; carry its own VPM layout too
     * (it differs from the render VS's - gl_Position only, no varyings). */
    req.coord_qpu_words         = h->coord_qpu_words;
    req.coord_num_uniforms      = h->coord_num_uniforms;
    req.coord_threads           = h->coord_threads;
    req.coord_single_seg        = h->coord_single_seg;
    req.coord_vpm_input_size    = h->coord_vpm_input_size;
    req.coord_vpm_output_size   = h->coord_vpm_output_size;
    req.coord_vcm_cache_size    = h->coord_vcm_cache_size;
    req.coord_separate_segments = h->coord_separate_segments;

    /* optional compute stage (VERSION 3): the fragment program as a CSD
     * kernel; cs_qpu_words == 0 means the program draws through the binner. */
    req.cs_qpu_words    = h->cs_qpu_words;
    req.cs_num_uniforms = h->cs_num_uniforms;
    req.cs_threads      = h->cs_threads;
    req.cs_single_seg   = h->cs_single_seg;
    req.cs_local_size[0] = h->cs_local_size[0];
    req.cs_local_size[1] = h->cs_local_size[1];
    req.cs_local_size[2] = h->cs_local_size[2];
    req.cs_shared_size  = h->cs_shared_size;
    req.cs_spill_size   = h->cs_spill_size;
    req.cs_is_geom      = h->cs_is_geom;
    req.cs_geom_tpw     = h->cs_geom_tpw;

    /* second compute stage (VERSION 4): the vertex-transform pass of the
     * two-pass geometry rasterizer; cs2_qpu_words == 0 means absent. */
    req.cs2_qpu_words    = h->cs2_qpu_words;
    req.cs2_num_uniforms = h->cs2_num_uniforms;
    req.cs2_threads      = h->cs2_threads;
    req.cs2_single_seg   = h->cs2_single_seg;
    req.cs2_local_size[0] = h->cs2_local_size[0];
    req.cs2_local_size[1] = h->cs2_local_size[1];
    req.cs2_local_size[2] = h->cs2_local_size[2];
    req.cs2_shared_size  = h->cs2_shared_size;
    req.cs2_uniform_blob_dwords = h->cs2_uniform_blob_dwords;

    {
        int32_t r = vc_pgl3d_program(&req);
        if (r < 0)
            slog("bsp_g3d: vc_pgl3d_program -> %d (vs %u fs %u coord %u cs %u)\n",
                 r, h->vs_qpu_words, h->fs_qpu_words, h->coord_qpu_words,
                 h->cs_qpu_words);
        return r;
    }
}

/* Release a program handle returned by bsp_g3d_program (its cache slot and
 * GPU dma go back for reuse).  g2dd calls this for clients that have exited. */
int32_t bsp_g3d_program_free(int32_t handle) {
    return vc_pgl3d_program_free(handle);
}
