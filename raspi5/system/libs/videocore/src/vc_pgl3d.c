/*
 * vc_pgl3d.c - VideoCore V3D 7.1 3D-pipeline back end for portablegl on the
 * raspi5 bsp_g2d layer.  See vc_pgl3d.h for the contract.
 *
 * This is a thin public layer over the private v3d_3d.c submission engine:
 * it brings the 3D pipeline up on top of the CSD path's V3D init, resolves
 * and re-validates each caller-supplied physical base into a GPU IOVA with
 * gpu_phys() (the same gate the 2D path uses), applies the fixed-function
 * eligibility subset, and forwards to v3d_3d_clear()/v3d_3d_draw().  All V3D
 * register access, cache maintenance and serialization stay in v3d_3d.c /
 * v3d_g2d.c.  A submitted dispatch is never replayed.
 */

#include <videocore/vc_g2d.h>
#include <videocore/vc_pgl3d.h>
#include "v3d_g2d.h"
#include "v3d_3d.h"

static int _pgl_ok;

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
    int tex_path = 0;

    if (!vc_pgl3d_probe() || req == NULL)
        return VC_PGL3D_UNSUPPORTED;

    if (req->w == 0 || req->h == 0 || req->stride < req->w * 4u)
        return VC_PGL3D_UNSUPPORTED;
    if (req->nverts == 0 || req->nverts > VC_PGL3D_MAX_VERTS)
        return VC_PGL3D_UNSUPPORTED;
    if (req->fmt != VC_PGL3D_FMT_RGBA8 && req->fmt != VC_PGL3D_FMT_BGR565)
        return VC_PGL3D_UNSUPPORTED;
    if (req->mode != VC_PGL3D_MODE_POINTS &&
        req->mode != VC_PGL3D_MODE_LINES &&
        req->mode != VC_PGL3D_MODE_TRIANGLES)
        return VC_PGL3D_UNSUPPORTED;

    /* Fixed-function eligibility subset: only the built-in solid / gouraud /
     * tex-modulate shaders run on the GPU (an arbitrary C fragment shader
     * stays on the CPU).  Depth test/write (persistent Z32F) and the full
     * back-face-cull matrix are rendered on the GPU, so every GL compare
     * function and cull config is passed through; blending is still declined
     * because the fixed-function frag has no blend stage. */
    if (req->fs_id != VC_PGL3D_FS_SOLID && req->fs_id != VC_PGL3D_FS_GOURAUD &&
        req->fs_id != VC_PGL3D_FS_TEX_MOD)
        return VC_PGL3D_UNSUPPORTED;
    if (req->depth_func > VC_PGL3D_DEPTH_NOTEQUAL)
        return VC_PGL3D_UNSUPPORTED;
    if (req->cull_en > VC_PGL3D_CULL_BOTH)
        return VC_PGL3D_UNSUPPORTED;
    if (req->blend_en)
        return VC_PGL3D_UNSUPPORTED;

    fb_iova  = gpu_phys(req->fb_phy,  (size_t)req->stride * req->h, req->fb_contig);
    pos_iova = gpu_phys(req->pos_phy, (size_t)req->nverts * 12u,    req->pos_contig);
    if (fb_iova == 0 || pos_iova == 0)
        return VC_PGL3D_UNSUPPORTED;

    /* TEX_MOD reads per-vertex uv (float3, stride 12) instead of a per-vertex
     * RGBA8 colour, and needs the linear RGBA8 texture image (row pitch
     * tex_stride, size tex_w x tex_h) to be resolvable to a GPU IOVA. */
    if (req->fs_id == VC_PGL3D_FS_TEX_MOD) {
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
            return VC_PGL3D_UNSUPPORTED;
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
    return v3d_3d_draw(fb_iova, req->stride, req->fmt, req->w, req->h,
                       (int)req->mode, req->nverts, pos_iova, col_iova,
                       req->rgba, req->rb_swap,
                       req->depth_func, req->depth_mask, req->cull_en,
                       z_iova, req->z_stride ? req->z_stride : req->w * 4u,
                       req->first_frame, req->clear_rgba, req->clear_z, &ext,
                       tex_path ? &tex : NULL);
}
