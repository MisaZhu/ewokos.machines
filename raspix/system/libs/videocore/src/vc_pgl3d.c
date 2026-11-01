/*
 * vc_pgl3d.c - VideoCore V3D 3D-pipeline back end for portablegl on the
 * raspix bsp_g2d layer.  See vc_pgl3d.h for the contract.
 *
 * This is a thin public layer over the private v3d_3d.c submission engine:
 * it brings the 3D pipeline up on top of the CSD path's V3D init, resolves
 * and re-validates each caller-supplied physical base with gpu_phys() (the
 * same gate the 2D path uses; on raspix it yields the physical address the
 * CSD fetches by, there being no V3D MMU page table), applies the eligibility
 * subset, and forwards to v3d_3d_clear()/v3d_3d_draw().  All V3D register
 * access, cache maintenance and serialization stay in v3d_3d.c / v3d_g2d.c,
 * which is also where the runtime version gate lives (v3d_3d_init declines
 * ver 21, Pi3).  A submitted dispatch is never replayed.
 */

#include <string.h>
#include <ewoksys/klog.h>
#include <videocore/vc_g2d.h>
#include <videocore/vc_pgl3d.h>
#include "v3d_g2d.h"
#include "v3d_3d.h"

static int _pgl_ok;

/* Compiled-draw decline at this layer: name the gate in /dev/log so the
 * client's bare -2 is attributable (fixed-function declines stay silent). */
static int decline(int compiled, const char *why)
{
    if (compiled)
        slog("vc_pgl3d: compiled draw declined: %s\r\n", why);
    return VC_PGL3D_UNSUPPORTED;
}

int vc_pgl3d_init(void)
{
    int r;

    if (_pgl_ok)
        return VC_PGL3D_DONE;

    /* The 3D pipeline reuses the CSD path's register window, V3D MMU,
     * SMS power-up, L2C enable and clock; v3d_g2d_init() is idempotent
     * (g2dd already ran it for the 2D ops) and never re-issues a runtime
     * power-domain write. */
    if (v3d_g2d_init() != 0 || !v3d_g2d_ready())
        return VC_PGL3D_UNSUPPORTED;

    r = v3d_3d_init();
    if (r != V3D_3D_DONE)
        return VC_PGL3D_UNSUPPORTED;

    _pgl_ok = 1;
    return VC_PGL3D_DONE;
}

int vc_pgl3d_probe(void)
{
    return _pgl_ok && v3d_3d_ready();
}

int vc_pgl3d_clear(ewokos_addr_t fb_phy, uint8_t contig, uint32_t stride,
                   uint32_t w, uint32_t h, uint32_t fmt, uint32_t rgba8,
                   int rb_swap)
{
    uint32_t fb_iova;

    if (!vc_pgl3d_probe())
        return VC_PGL3D_UNSUPPORTED;
    if (w == 0 || h == 0 || stride < w * 4u)
        return VC_PGL3D_UNSUPPORTED;
    if (fmt != VC_PGL3D_FMT_RGBA8 && fmt != VC_PGL3D_FMT_BGR565)
        return VC_PGL3D_UNSUPPORTED;

    fb_iova = gpu_phys(fb_phy, (size_t)stride * h, contig);
    if (fb_iova == 0)
        return VC_PGL3D_UNSUPPORTED;

    /* V3D_3D_* and VC_PGL3D_* tri-states are value-identical. */
    return v3d_3d_clear(fb_iova, stride, fmt, w, h, rgba8, rb_swap);
}

int vc_pgl3d_draw(const vc_pgl3d_draw_req_t *req)
{
    uint32_t fb_iova, pos_iova, col_iova, z_iova;
    v3d_3d_ext_t ext;
    v3d_3d_tex_t tex;
    v3d_3d_ctex_t ctex;
    int tex_path = 0;
    int ctex_path = 0;
    int compiled;

    if (!vc_pgl3d_probe() || req == NULL)
        return VC_PGL3D_UNSUPPORTED;
    compiled = (req->prog_handle >= 0);

    if (req->w == 0 || req->h == 0 || req->stride < req->w * 4u)
        return decline(compiled, "w/h/stride");
    if (req->nverts == 0 || req->nverts > VC_PGL3D_MAX_VERTS)
        return decline(compiled, "nverts");
    if (req->fmt != VC_PGL3D_FMT_RGBA8 && req->fmt != VC_PGL3D_FMT_BGR565)
        return decline(compiled, "fmt");
    if (req->mode != VC_PGL3D_MODE_POINTS &&
        req->mode != VC_PGL3D_MODE_LINES &&
        req->mode != VC_PGL3D_MODE_TRIANGLES)
        return decline(compiled, "mode");

    /* Fixed-function eligibility subset: only the built-in solid / gouraud /
     * tex-modulate shaders run on the GPU (an arbitrary C fragment shader
     * stays on the CPU).  Depth test/write (persistent Z32F) and the full
     * back-face-cull matrix are rendered on the GPU, so every GL compare
     * function and cull config is passed through; blending is still declined
     * because the fixed-function frag has no blend stage.  A compiled draw
     * (prog_handle >= 0) supplies its own mesa-compiled VS/FS QPU, whose
     * eligibility was already decided at upload time, so the fs_id gate is
     * skipped; the depth / cull / blend gates still apply (the compiled path
     * reuses the same fixed-function render state). */
    if (!compiled) {
        if (req->fs_id != VC_PGL3D_FS_SOLID && req->fs_id != VC_PGL3D_FS_GOURAUD &&
            req->fs_id != VC_PGL3D_FS_TEX_MOD)
            return VC_PGL3D_UNSUPPORTED;
    }
    if (req->depth_func > VC_PGL3D_DEPTH_NOTEQUAL)
        return decline(compiled, "depth_func");
    if (req->cull_en > VC_PGL3D_CULL_BOTH)
        return decline(compiled, "cull_en");
    if (req->blend_en)
        return decline(compiled, "blend_en");

    fb_iova  = gpu_phys(req->fb_phy,  (size_t)req->stride * req->h, req->fb_contig);
    pos_iova = gpu_phys(req->pos_phy, (size_t)req->nverts * 12u,    req->pos_contig);
    if (fb_iova == 0 || pos_iova == 0)
        return decline(compiled, "fb/pos gpu_phys");

    /* TEX_MOD reads per-vertex uv (float3, stride 12) instead of a per-vertex
     * RGBA8 colour, and needs the linear RGBA8 texture image (row pitch
     * tex_stride, size tex_w x tex_h) to be resolvable to a GPU IOVA. */
    if (!compiled && req->fs_id == VC_PGL3D_FS_TEX_MOD) {
        uint32_t tex_stride, uv_iova, tex_iova;
        if (req->tex_phy == 0 || req->uv_phy == 0 ||
            req->tex_w == 0 || req->tex_h == 0)
            return VC_PGL3D_UNSUPPORTED;
        tex_stride = req->tex_stride ? req->tex_stride : req->tex_w * 4u;
        if (tex_stride < req->tex_w * 4u)
            return VC_PGL3D_UNSUPPORTED;
        uv_iova  = gpu_phys(req->uv_phy,  (size_t)req->nverts * 12u, req->uv_contig);
        tex_iova = gpu_phys(req->tex_phy, (size_t)tex_stride * req->tex_h,
                            req->tex_contig);
        if (uv_iova == 0 || tex_iova == 0)
            return VC_PGL3D_UNSUPPORTED;
        tex.tex_iova   = tex_iova;
        tex.tex_stride = tex_stride;
        tex.tex_w      = req->tex_w;
        tex.tex_h      = req->tex_h;
        tex.uv_iova    = uv_iova;
        tex_path = 1;
        /* col_phy is unused on the tex path; a caller that leaves it zero is
         * fine.  When non-zero, still resolve it so the driver's own validity
         * check does not reject the draw. */
        col_iova = req->col_phy
            ? gpu_phys(req->col_phy, (size_t)req->nverts * 4u, req->col_contig)
            : 0;
        if (req->col_phy != 0 && col_iova == 0)
            return VC_PGL3D_UNSUPPORTED;
    } else if (compiled) {
        /* The compiled path reads clip-space pos (attr 0); col is optional.  A
         * pos-only full-screen quad (shadertoy) supplies none (col_phy == 0 ->
         * col_iova == 0 -> the back end builds one attribute record).  A
         * de-interleaved mesh (teapot) supplies the attr-1 NORMAL float3 stream
         * here, so resolve it over nverts*12 bytes (not the fixed path's byte4
         * nverts*4); the back end then builds two float3 attribute records. */
        col_iova = req->col_phy
            ? gpu_phys(req->col_phy, (size_t)req->nverts * 12u, req->col_contig)
            : 0;
        if (req->col_phy != 0 && col_iova == 0)
            return decline(1, "col/normal gpu_phys");
        memset(&tex, 0, sizeof(tex));
    } else {
        col_iova = gpu_phys(req->col_phy, (size_t)req->nverts * 4u, req->col_contig);
        if (col_iova == 0)
            return VC_PGL3D_UNSUPPORTED;
        memset(&tex, 0, sizeof(tex));
    }

    /* the persistent Z surface is optional: z_phy == 0 (no depth, or the
     * client could not allocate it) falls back to tile-local Z.  Its stride
     * defaults to w*4 (linear Z32F) when the client did not send one. */
    z_iova = 0;
    if (req->z_phy != 0) {
        uint32_t z_stride = req->z_stride ? req->z_stride : req->w * 4u;
        z_iova = gpu_phys(req->z_phy, (size_t)z_stride * req->h, req->z_contig);
        if (z_iova == 0)
            return decline(compiled, "z gpu_phys");
    }

    /* VC_PGL3D_DEPTH_* and V3D_3D_DEPTH_* are value-identical.  The Phase B2c
     * scissor / polygon-offset / colour-mask state is copied verbatim into the
     * driver's ext struct (all-zero = scissor-off / no-offset / write-all). */
    ext.scissor_x  = req->scissor_x;
    ext.scissor_y  = req->scissor_y;
    ext.scissor_w  = req->scissor_w;
    ext.scissor_h  = req->scissor_h;
    ext.poly_enable = req->poly_enable;
    ext.poly_factor = req->poly_factor;
    ext.poly_units  = req->poly_units;
    ext.color_mask  = req->color_mask & 0xFu;

    /* WS4 compiled-shader sampler bindings: resolve each bound unit's
     * physical base into a GPU IOVA (same gate the 2D and TEX_MOD paths use)
     * and hand the resulting v3d_3d_ctex_t to v3d_3d_draw so expand_uniforms
     * can bind it at the TMU_CONFIG slot the compiled FS references.  A slot
     * with ctex_phy == 0 stays unbound (ctex_iova == 0 through the memset).
     * Only the compiled path (prog_handle >= 0) uses this block; a
     * fixed-function draw leaves ctex_path == 0 and passes NULL. */
    memset(&ctex, 0, sizeof(ctex));
    if (compiled) {
        uint32_t i;
        for (i = 0; i < VC_PGL3D_MAX_TEX_UNITS; i++) {
            uint32_t stride_i, iova_i;
            if (req->ctex_phy[i] == 0)
                continue;
            if (req->ctex_w[i] == 0 || req->ctex_h[i] == 0)
                return decline(1, "ctex w/h");
            stride_i = req->ctex_stride[i] ? req->ctex_stride[i]
                                           : req->ctex_w[i] * 4u;
            if (stride_i < req->ctex_w[i] * 4u)
                return decline(1, "ctex stride");
            iova_i = gpu_phys(req->ctex_phy[i],
                              (size_t)stride_i * req->ctex_h[i],
                              req->ctex_contig[i]);
            if (iova_i == 0)
                return decline(1, "ctex gpu_phys");
            ctex.ctex_iova[i]   = iova_i;
            ctex.ctex_stride[i] = stride_i;
            ctex.ctex_w[i]      = req->ctex_w[i];
            ctex.ctex_h[i]      = req->ctex_h[i];
            ctex.ctex_tiling[i] = req->ctex_tiling[i];
            ctex.ctex_ub_pad[i] = req->ctex_ub_pad[i];
            ctex_path = 1;
        }
    }

    return v3d_3d_draw(fb_iova, req->stride, req->fmt, req->w, req->h,
                       (int)req->mode, req->nverts, pos_iova, col_iova,
                       req->rgba, req->rb_swap,
                       req->depth_func, req->depth_mask, req->cull_en,
                       z_iova, req->z_stride ? req->z_stride : req->w * 4u,
                       req->first_frame, req->clear_rgba, req->clear_z, &ext,
                       tex_path ? &tex : NULL,
                       req->prog_handle, req->unif, req->num_uniforms,
                       ctex_path ? &ctex : NULL);
}

/* Upload a compiled program (VS/FS QPU + uniform streams + prog_data) into GPU
 * dma and cache it by handle.  Thin public layer over the private
 * v3d_3d_program: it probe-gates, then translates the public request into the
 * private v3d_3d_prog_desc_t field-by-field (the same boundary vc_pgl3d_draw
 * keeps so v3d_3d.h never leaks into bsp_g3d) and forwards.  VC_PGL3D_PROG_*
 * and V3D_3D_PROG_* codes are value-identical. */
int vc_pgl3d_program(const vc_pgl3d_program_req_t *req)
{
    v3d_3d_prog_desc_t d;

    if (!vc_pgl3d_probe() || req == NULL)
        return VC_PGL3D_PROG_UNSUPPORTED;

    memset(&d, 0, sizeof(d));
    /* vertex stage */
    d.vs_qpu               = req->vs_qpu;
    d.vs_qpu_words         = req->vs_qpu_words;
    d.vs_unif_contents     = req->vs_unif_contents;
    d.vs_unif_data         = req->vs_unif_data;
    d.vs_num_uniforms      = req->vs_num_uniforms;
    d.vs_threads           = req->vs_threads;
    d.vs_single_seg        = req->vs_single_seg;
    d.vs_vpm_input_size    = req->vs_vpm_input_size;
    d.vs_vpm_output_size   = req->vs_vpm_output_size;
    d.vs_vcm_cache_size    = req->vs_vcm_cache_size;
    d.vs_separate_segments = req->vs_separate_segments;
    memcpy(d.vs_vattr_sizes, req->vs_vattr_sizes, sizeof(d.vs_vattr_sizes));
    memcpy(d.vs_driver_location_map, req->vs_driver_location_map,
           sizeof(d.vs_driver_location_map));
    /* fragment stage */
    d.fs_qpu               = req->fs_qpu;
    d.fs_qpu_words         = req->fs_qpu_words;
    d.fs_unif_contents     = req->fs_unif_contents;
    d.fs_unif_data         = req->fs_unif_data;
    d.fs_num_uniforms      = req->fs_num_uniforms;
    d.fs_threads           = req->fs_threads;
    d.fs_single_seg        = req->fs_single_seg;
    d.fs_needs_coords      = req->fs_needs_coords;
    d.fs_uses_center_w     = req->fs_uses_center_w;
    d.fs_writes_z          = req->fs_writes_z;
    d.fs_disable_ez        = req->fs_disable_ez;
    d.fs_num_inputs        = req->fs_num_inputs;
    memcpy(d.fs_input_slots, req->fs_input_slots, sizeof(d.fs_input_slots));
    /* coordinate (binning) stage */
    d.coord_qpu               = req->coord_qpu;
    d.coord_qpu_words         = req->coord_qpu_words;
    d.coord_unif_contents     = req->coord_unif_contents;
    d.coord_unif_data         = req->coord_unif_data;
    d.coord_num_uniforms      = req->coord_num_uniforms;
    d.coord_threads           = req->coord_threads;
    d.coord_single_seg        = req->coord_single_seg;
    d.coord_vpm_input_size    = req->coord_vpm_input_size;
    d.coord_vpm_output_size   = req->coord_vpm_output_size;
    d.coord_vcm_cache_size    = req->coord_vcm_cache_size;
    d.coord_separate_segments = req->coord_separate_segments;
    /* optional compute stage */
    d.cs_qpu           = req->cs_qpu;
    d.cs_qpu_words     = req->cs_qpu_words;
    d.cs_unif_contents = req->cs_unif_contents;
    d.cs_unif_data     = req->cs_unif_data;
    d.cs_num_uniforms  = req->cs_num_uniforms;
    d.cs_threads       = req->cs_threads;
    d.cs_single_seg    = req->cs_single_seg;
    d.cs_local_size[0] = req->cs_local_size[0];
    d.cs_local_size[1] = req->cs_local_size[1];
    d.cs_local_size[2] = req->cs_local_size[2];
    d.cs_shared_size   = req->cs_shared_size;
    d.cs_spill_size    = req->cs_spill_size;
    d.cs_is_geom       = req->cs_is_geom;
    d.cs_geom_tpw      = req->cs_geom_tpw;
    /* second compute stage (VERSION 4): the vertex-transform pass */
    d.cs2_qpu           = req->cs2_qpu;
    d.cs2_qpu_words     = req->cs2_qpu_words;
    d.cs2_unif_contents = req->cs2_unif_contents;
    d.cs2_unif_data     = req->cs2_unif_data;
    d.cs2_num_uniforms  = req->cs2_num_uniforms;
    d.cs2_threads       = req->cs2_threads;
    d.cs2_single_seg    = req->cs2_single_seg;
    d.cs2_local_size[0] = req->cs2_local_size[0];
    d.cs2_local_size[1] = req->cs2_local_size[1];
    d.cs2_local_size[2] = req->cs2_local_size[2];
    d.cs2_shared_size   = req->cs2_shared_size;
    d.cs2_uniform_blob_dwords = req->cs2_uniform_blob_dwords;
    d.cs2_vout_stride  = req->cs2_vout_stride;
    d.vk_attr_stride   = req->vk_attr_stride;

    return v3d_3d_program(&d);
}

/* Release a cached program.  Probe-gated like the upload so a machine whose
 * 3D path never came up answers UNSUPPORTED instead of poking the table. */
int vc_pgl3d_program_free(int handle)
{
    if (!vc_pgl3d_probe())
        return VC_PGL3D_PROG_UNSUPPORTED;
    return v3d_3d_program_free(handle);
}
