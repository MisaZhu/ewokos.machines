/*
 * portablegl geometry rasterizer on GA10B: the daemon half (bsp_g3d back
 * end) and the wire blob it shares with the Orin pglcompiler back end.
 *
 * A "program" is a pair of precompiled compute kernels (vertex transform +
 * raster) selected by id; the kernels themselves are built offline with
 * nvcc and embedded in the driver library.  The uniform block the client
 * sends per draw is [xform region | raster region], split at
 * xform_unif_dwords.
 */
#ifndef GA10B_PGL_H
#define GA10B_PGL_H

#include <stdint.h>

/* value bsp_g2d_version() reports / pglc_version_supported() accepts */
#define GA10B_PGL_VERSION      1087u

#define GA10B_PROG_MAGIC       0x3347564eu   /* 'N','V','G','3' */
#define GA10B_PROG_VERSION     1u

/* kernel ids (the "code" a compiled stage carries), one per kernel text of
 * geom_raster_cs.h */
#define GA10B_PGL_K_NONE          0u
#define GA10B_PGL_K_XFORM_COLOR   1u   /* geom_xform_color_cs_glsl */
#define GA10B_PGL_K_RASTER        2u   /* geom_raster_cs_glsl */
#define GA10B_PGL_K_XFORM_LIT     3u   /* geom_xform_cs_glsl */
#define GA10B_PGL_K_XFORM_TEX     4u   /* geom_xform_tex_cs_glsl */
#define GA10B_PGL_K_XFORM_TEXCOL  5u   /* geom_xform_texcol_cs_glsl */
#define GA10B_PGL_K_XFORM_INST    6u   /* geom_xform_inst_cs_glsl */
#define GA10B_PGL_K_XFORM_INSTLIT 7u   /* geom_xform_instlit_cs_glsl */
#define GA10B_PGL_K_RASTER_TEX    8u   /* geom_raster_tex_cs_glsl */
#define GA10B_PGL_K_RASTER_TEXCOL 9u   /* geom_raster_texcol_cs_glsl */

/* geom_kind values of a serialized program (pglc_blob_opts.geom_kind) */
#define GA10B_PGL_KIND_GOURAUD    1u
#define GA10B_PGL_KIND_TEX        2u
#define GA10B_PGL_KIND_INST       3u
#define GA10B_PGL_KIND_TEXCOL     4u

/* uniform word offsets inside each kernel's region (kernels/pgl_geom.cu).
 * colour / textured / colour-modulated vertex transforms share the head. */
#define GA10B_PGL_XF_MVP          0u
#define GA10B_PGL_XF_VIEWPORT_WH  16u
#define GA10B_PGL_XF_VERT_COUNT   18u
#define GA10B_PGL_XF_COL_ALPHA    19u
#define GA10B_PGL_XF_DWORDS       20u
/* lit gouraud vertex transform */
#define GA10B_PGL_XL_MVP          0u
#define GA10B_PGL_XL_MODELVIEW    16u
#define GA10B_PGL_XL_NORMALMAT    32u
#define GA10B_PGL_XL_MATERIAL     48u
#define GA10B_PGL_XL_LIGHT_EYE    52u
#define GA10B_PGL_XL_SHADE        56u
#define GA10B_PGL_XL_VIEWPORT_WH  60u
#define GA10B_PGL_XL_VERT_COUNT   62u
#define GA10B_PGL_XL_DWORDS       64u
/* instanced vertex transforms */
#define GA10B_PGL_XI_LIGHT_EYE    0u
#define GA10B_PGL_XI_SHADE        4u
#define GA10B_PGL_XI_VIEWPORT_WH  8u
#define GA10B_PGL_XI_VERT_COUNT   10u
#define GA10B_PGL_XI_DWORDS       12u

/* raster kernels: the first five words are common to all three */
#define GA10B_PGL_RS_VIEWPORT_WH  0u
#define GA10B_PGL_RS_FB_STRIDE    2u
#define GA10B_PGL_RS_TRI_FIRST    3u
#define GA10B_PGL_RS_TRI_COUNT    4u
#define GA10B_PGL_RS_BLEND_MODE   5u   /* colour raster */
#define GA10B_PGL_RS_DEPTH_WRITE  6u
#define GA10B_PGL_RS_CULL_SIGN    7u
#define GA10B_PGL_RS_DWORDS       8u
#define GA10B_PGL_RT_TEX_W        5u   /* textured rasters */
#define GA10B_PGL_RT_TEX_H        6u
#define GA10B_PGL_RT_DWORDS       8u
#define GA10B_PGL_RT_SCISSOR      7u   /* colour-modulated raster: x,y,w,h */
#define GA10B_PGL_RTC_DWORDS      12u

/* tile-pass parameter block (BP_* in kernels/pgl_geom.cu) */
#define GA10B_PGL_BP_WORDS        18u
#define GA10B_PGL_TILE            16u

/* Fill the tile-pass parameter block for one draw from the raster kernel
 * id and its uniform region.  Shared by the daemon and its tests so both
 * derive the kernel state the same way.  Returns 0, or -1 for an unknown
 * raster kernel. */
static inline int ga10b_pgl_fill_bp(uint32_t* bp, uint32_t raster_kernel,
                                    const uint32_t* ru, uint32_t tiles_x,
                                    uint32_t tiles_y, uint32_t cap, int clear,
                                    uint32_t clear_argb) {
    for (uint32_t i = 0; i < GA10B_PGL_BP_WORDS; i++)
        bp[i] = 0;
    bp[0] = tiles_x;
    bp[1] = tiles_y;
    bp[2] = cap;
    bp[3] = clear ? 1u : 0u;
    bp[4] = clear_argb;
    bp[5] = 0xffffffffu;                      /* far depth */
    switch (raster_kernel) {
    case GA10B_PGL_K_RASTER:
        bp[6] = 0; bp[7] = 8; bp[8] = 4;      /* mode, vout stride, attrs */
        bp[9] = ru[GA10B_PGL_RS_CULL_SIGN];
        bp[10] = ru[GA10B_PGL_RS_BLEND_MODE];
        bp[11] = ru[GA10B_PGL_RS_DEPTH_WRITE];
        return 0;
    case GA10B_PGL_K_RASTER_TEX:
        bp[6] = 1; bp[7] = 8; bp[8] = 2;
        bp[11] = 1;                           /* always test + write */
        bp[12] = ru[GA10B_PGL_RT_TEX_W];
        bp[13] = ru[GA10B_PGL_RT_TEX_H];
        return 0;
    case GA10B_PGL_K_RASTER_TEXCOL:
        bp[6] = 2; bp[7] = 12; bp[8] = 6;
        bp[12] = ru[GA10B_PGL_RT_TEX_W];
        bp[13] = ru[GA10B_PGL_RT_TEX_H];
        for (uint32_t i = 0; i < 4; i++)
            bp[14 + i] = ru[GA10B_PGL_RT_SCISSOR + i];
        return 0;
    default:
        return -1;
    }
}

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t geom_kind;           /* GA10B_PGL_KIND_* */
    uint32_t geom_tpw;            /* triangles per raster block */
    uint32_t raster_kernel;       /* GA10B_PGL_K_* */
    uint32_t xform_kernel;
    uint32_t raster_unif_dwords;
    uint32_t xform_unif_dwords;
} ga10b_prog_blob_t;

/* results: 0 ok; DECLINED = nothing was submitted (the client may replay on
 * the CPU); UNSUPPORTED = this back end cannot do it at all; TIMEOUT = a
 * submitted launch never retired (the surface state is unknown). */
#define GA10B_PGL_OK             0
#define GA10B_PGL_E_DECLINED     (-1)
#define GA10B_PGL_E_UNSUPPORTED  (-2)
#define GA10B_PGL_E_TIMEOUT      (-3)

typedef struct {
    uint64_t fb_phy;
    uint32_t w, h, stride;        /* stride in bytes */
    uint32_t nverts;
    uint64_t pos_phy, col_phy;
    uint64_t z_phy;
    uint32_t z_stride;
    /* sampler unit 0: the texture image (textured kinds, linear ARGB rows)
       or the per-instance record stream (instanced kind) */
    uint64_t ctex_phy;
    uint32_t ctex_w, ctex_h, ctex_stride;
    int      first_frame;
    uint32_t clear_rgba;          /* portablegl 0xAABBGGRR */
} ga10b_pgl_draw_t;

/* full GPU bring-up + kernel upload; idempotent, 0 when usable */
int ga10b_pgl_init(void);
int ga10b_pgl_ready(void);
/* log average per-stage wall time every 16 draws */
void ga10b_pgl_set_profile(int on);
/* fill a w x h ARGB8888 surface (tight stride) with one word */
int ga10b_pgl_clear(uint64_t fb_phy, uint32_t w, uint32_t h, uint32_t stride,
                    uint32_t argb);
/* returns a handle >= 0 or a GA10B_PGL_E_* code */
int ga10b_pgl_program_load(const void* blob, uint32_t size);
int ga10b_pgl_program_free(int handle);
int ga10b_pgl_draw(int handle, const ga10b_pgl_draw_t* d,
                   const uint32_t* unif, uint32_t num_uniforms);

#endif
