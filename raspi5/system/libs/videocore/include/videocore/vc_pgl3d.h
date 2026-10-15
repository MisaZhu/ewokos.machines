/*
 * vc_pgl3d.h - VideoCore V3D 7.1 3D-pipeline back end for portablegl on the
 * raspi5 (Raspberry Pi 5 / BCM2712) bsp_g2d layer.
 *
 * This is the public contract between bsp_g2d.c (the /dev/g2d daemon side,
 * argument validation and physical-base resolution) and v3d_3d.c (the CLE
 * binning + render submission mechanics, kept private to libvideocore).  It
 * exposes the two 3D device operations portablegl drives through the
 * stateless g2dclient shm-canvas protocol: a full-frame clear and a
 * fixed-function primitive draw.
 *
 * Division of labour (see the plan's Architecture decisions): the CPU runs
 * portablegl's ARBITRARY vertex shader per vertex and hands this layer the
 * resulting clip-space positions plus a per-vertex colour; the GPU owns
 * rasterization, interpolation, the BUILT-IN fixed-function fragment shader,
 * and the store.  Arbitrary C fragment shaders are never sent here - the
 * caller keeps them on the CPU.
 *
 * ZERO COPY: like the CSD 2D path, the GPU operates directly on the caller's
 * contig-shm / dma buffers through their resolved physical bases (IOVA = low
 * 32 bits of phys); nothing is copied.
 *
 * Every entry point returns a DONE/FAILED/UNSUPPORTED tri-state.  A submitted
 * dispatch is NEVER replayed (a timed-out job may still own the destination),
 * so FAILED and UNSUPPORTED both mean "the caller must fall back to its CPU
 * path for this and - via the sticky latch - subsequent frames".
 *
 * HARDWARE STATUS: the V3D 7.1 binning/render bring-up is experimental and
 * cannot be exercised under QEMU (no V3D 3D model); it requires validation on
 * a physical Pi 5.  Depth test/write (tile-local Z) and back-face culling are
 * rendered on the GPU for the built-in solid / gouraud fragment shaders; the
 * exact CFG_BITS depth-function bit placement is confirmed iteratively on the
 * Pi 5.  Blending and arbitrary C fragment shaders are still declined
 * (UNSUPPORTED) rather than drawn incorrectly.
 */

#ifndef VIDEOCORE_VC_PGL3D_H
#define VIDEOCORE_VC_PGL3D_H

#include <stdint.h>
#include <stddef.h>
#include <ewoksys/ewokdef.h>

/* Tri-state result (matches the V3D_3D_* values and the g2d result codes'
 * "not mine" spirit: DONE = drawn, the other two = use the CPU path). */
#define VC_PGL3D_DONE        0
#define VC_PGL3D_FAILED      1
#define VC_PGL3D_UNSUPPORTED 2
/* compiled-draw refinements of FAILED (V3D_3D_TIMEOUT / V3D_3D_MMU_FAULT):
 * the job was submitted but overran its wait budget / faulted the GPU MMU. */
#define VC_PGL3D_TIMEOUT     3
#define VC_PGL3D_MMU_FAULT   4

/* Output image formats (V3D STORE_TILE_BUFFER_GENERAL field values). */
#define VC_PGL3D_FMT_BGR565 7u
#define VC_PGL3D_FMT_RGBA8  27u

/* Primitive modes (V3D VERTEX_ARRAY_PRIMS field values). */
#define VC_PGL3D_MODE_POINTS    0
#define VC_PGL3D_MODE_LINES     1
#define VC_PGL3D_MODE_TRIANGLES 4

/* Built-in fixed-function fragment-shader ids.  Only these run on the GPU;
 * any other portablegl fragment shader stays on the CPU.  SOLID emits a
 * constant colour, GOURAUD emits the interpolated per-vertex colour, TEX_MOD
 * samples a linear RGBA8 texture with point filtering and CLAMP-to-edge
 * addressing (Phase D: the 3ddemo opt-in). */
enum {
    VC_PGL3D_FS_SOLID   = 0,
    VC_PGL3D_FS_GOURAUD = 1,
    VC_PGL3D_FS_TEX_MOD = 2
};

/* Depth-function codes carried by a draw request.  NONE disables the depth
 * test (and leaves the tile-local Z buffer unallocated); the rest select the
 * V3D depth_test_function (Compare Function enum) and turn the Z buffer on.
 * All eight GL compare functions are accepted for the built-in solid / gouraud
 * shaders; a code above VC_PGL3D_DEPTH_NOTEQUAL is declined.  Value-identical
 * to the V3D_3D_DEPTH_* / G2D_PGL_DEPTH_* / PGL_G3D_DEPTH_* sets. */
enum {
    VC_PGL3D_DEPTH_NONE = 0,
    VC_PGL3D_DEPTH_LESS,
    VC_PGL3D_DEPTH_LEQUAL,
    VC_PGL3D_DEPTH_GREATER,
    VC_PGL3D_DEPTH_GEQUAL,
    VC_PGL3D_DEPTH_ALWAYS,
    VC_PGL3D_DEPTH_NEVER,
    VC_PGL3D_DEPTH_EQUAL,
    VC_PGL3D_DEPTH_NOTEQUAL
};

/* Back-face-cull configuration codes carried in cull_en.  0 draws both facings;
 * the rest pick which facing(s) survive plus the front-face winding (decoded
 * into CFG_BITS bits 0-2).  Value-identical to the V3D_3D_CULL_* set. */
enum {
    VC_PGL3D_CULL_NONE = 0,
    VC_PGL3D_CULL_BACK_CCW,
    VC_PGL3D_CULL_BACK_CW,
    VC_PGL3D_CULL_FRONT_CCW,
    VC_PGL3D_CULL_FRONT_CW,
    VC_PGL3D_CULL_BOTH
};

/* Maximum vertices per single draw dispatch (larger draws must be split by
 * the caller; a bigger count is declined). */
#define VC_PGL3D_MAX_VERTS 4096u

/* Maximum texture units a compiled FS may bind (iChannel0..iChannelN).
 * Mirrors G2D_PGL_MAX_TEX_UNITS on the client protocol side and
 * V3D_MAX_TEXTURE_SAMPLERS in the private v3d_3d layer. */
#define VC_PGL3D_MAX_TEX_UNITS 4u

/* One draw request, filled by bsp_g2d from the g2dclient shm command buffer.
 * The *_phy fields carry the resolved physical base of a contig buffer and
 * the matching *_contig flag is non-zero when that base is valid (mirrors the
 * bsp_g2d fill/blit contract); this layer re-validates each with gpu_phys(). */
typedef struct {
    uint32_t      mode;         /* VC_PGL3D_MODE_* */
    uint32_t      nverts;
    ewokos_addr_t pos_phy;      /* float3 clip-space positions, stride 12 */
    uint8_t       pos_contig;
    ewokos_addr_t col_phy;      /* byte4 RGBA8 per vertex, stride 4 */
    uint8_t       col_contig;
    ewokos_addr_t fb_phy;       /* render target (RGBA8 / BGR565) */
    uint8_t       fb_contig;
    uint32_t      stride;       /* render-target row pitch, bytes */
    uint32_t      w, h;         /* render-target size, pixels */
    uint32_t      fmt;          /* VC_PGL3D_FMT_* */
    uint32_t      fs_id;        /* VC_PGL3D_FS_* */
    uint32_t      rgba;         /* solid colour 0xAABBGGRR (FS_SOLID) */
    int           rb_swap;      /* swap R/B on store (BGR565 panel) */
    /* state subset carried for the protocol: depth test/write (tile-local Z)
     * and back-face cull are rendered on the GPU; blending is still declined
     * (the fixed-function frag has no blend stage). */
    uint32_t      depth_func;   /* VC_PGL3D_DEPTH_* */
    uint8_t       depth_mask;   /* depth writes enabled */
    uint8_t       blend_en;     /* blending enabled (declined) */
    uint8_t       cull_en;      /* VC_PGL3D_CULL_* config code (0 = no cull) */
    /* persistent depth / multi-dispatch frames: a linear Z32F surface that
     * survives across dispatches so depth (and colour) accumulate within a
     * frame.  z_phy == 0 falls back to tile-local Z.  first_frame marks the
     * frame's first dispatch (clears colour to clear_rgba + Z to clear_z);
     * later dispatches LOAD the persisted colour+Z. */
    ewokos_addr_t z_phy;        /* Z32F depth surface (linear, stride z_stride) */
    uint8_t       z_contig;
    uint32_t      z_stride;     /* Z row pitch, bytes (w * 4) */
    uint8_t       first_frame;  /* non-zero: first dispatch of the frame */
    uint32_t      clear_rgba;   /* colour the first dispatch clears to */
    float         clear_z;      /* depth the first dispatch clears to */
    /* Phase B2c extended render state, forwarded verbatim to v3d_3d_draw:
     * scissor clip window, polygon offset and the colour write mask.  The
     * scissor_* fields are already in the render target's y-down orientation
     * (scissor_y = rows from the top edge to the clip window's bottom row);
     * scissor_w/h == 0 disables the scissor (full frame).  poly_enable gates
     * the polygon offset (poly_factor / poly_units, packed as f187 downstream).
     * color_mask is the GL RGBA write-enable nibble (bit0=R..bit3=A); 0xF (or
     * 0) writes all four channels.  Every field defaults to zero through the
     * request memset, which is exactly scissor-off / no-offset / write-all. */
    int32_t       scissor_x, scissor_y, scissor_w, scissor_h;
    uint8_t       poly_enable;
    float         poly_factor, poly_units;
    uint32_t      color_mask;
    /* Phase D texture state (fs_id == VC_PGL3D_FS_TEX_MOD only): the linear
     * RGBA8 texture image, its row pitch and size, plus the per-vertex uv
     * float3 attribute (stride 12, third component unused).  Every field
     * defaults to zero through the request memset, which makes tex_phy == 0
     * decline the tex path back to UNSUPPORTED for a misconfigured caller. */
    ewokos_addr_t tex_phy;      /* linear RGBA8 texture image (row pitch tex_stride) */
    uint8_t       tex_contig;
    uint32_t      tex_stride;   /* texture row pitch, bytes (tex_w * 4) */
    uint32_t      tex_w, tex_h; /* texture size, texels */
    ewokos_addr_t uv_phy;       /* per-vertex uv float3 (stride 12) */
    uint8_t       uv_contig;
    /* WS4 compiled-shader draw reference.  prog_handle >= 0 selects an
     * on-device-compiled program (uploaded via vc_pgl3d_program and cached by
     * the private v3d_3d layer); the back end then builds the V3D shader-state
     * record from the cached prog_data and dispatches the compiled VS/FS QPU
     * instead of the fixed-function fs_id shaders.  `unif` points to
     * num_uniforms u32 per-draw GL uniform values (the mesa job->uniforms
     * storage) that the QUNIFORM interpreter indexes for QUNIFORM_UNIFORM
     * slots; it is NULL when num_uniforms == 0.  prog_handle < 0 (the memset
     * default is -1 from the protocol) keeps the historical fixed-function
     * path, so every existing caller is unaffected. */
    int32_t        prog_handle;
    const uint32_t *unif;
    uint32_t       num_uniforms;
    /* WS4 compiled-shader sampler bindings.  A compiled FS may reference up to
     * VC_PGL3D_MAX_TEX_UNITS texture units; each unit's QUNIFORM_TMU_CONFIG_P0/P1
     * pair carries the unit index in the upper 8 bits of the data word, so
     * v3d_3d's expand_uniforms walks the FS uniform stream, extracts each
     * referenced unit index and binds the corresponding ctex_phy[i] image at
     * that TMU slot.  A slot with ctex_phy[i] == 0 is unbound: the FS's sample
     * returns whatever the driver leaves in the TMU config (safe black for
     * shaders that never sample it).  Images are RGBA8 in the V3D texture
     * memory layout named by ctex_tiling[i] (0 raster, 1 LT, 2/3 UBLINEAR,
     * 4/5 UIF no-xor/xor; ctex_ub_pad[i] is the UIF level_0_ub_pad), contig
     * or dma so the GPU can address them by physical base; ctex_stride[i] is
     * the allocated (padded) row pitch, 0 lets the driver derive
     * ctex_w[i] * 4.  Only meaningful when prog_handle >= 0.
     * Every field defaults to zero through the request memset, so existing
     * callers that never bind a texture are unaffected. */
    ewokos_addr_t ctex_phy[VC_PGL3D_MAX_TEX_UNITS];
    uint8_t       ctex_contig[VC_PGL3D_MAX_TEX_UNITS];
    uint32_t      ctex_stride[VC_PGL3D_MAX_TEX_UNITS];
    uint32_t      ctex_w[VC_PGL3D_MAX_TEX_UNITS];
    uint32_t      ctex_h[VC_PGL3D_MAX_TEX_UNITS];
    uint8_t       ctex_tiling[VC_PGL3D_MAX_TEX_UNITS];
    uint8_t       ctex_ub_pad[VC_PGL3D_MAX_TEX_UNITS];
} vc_pgl3d_draw_req_t;

/* Bring up the 3D pipeline on top of the CSD path's V3D init (idempotent):
 * ensures v3d_g2d is ready, then allocates the 3D dma staging.  Returns
 * VC_PGL3D_DONE on success, VC_PGL3D_UNSUPPORTED when the GPU cannot be
 * used for 3D (the caller then keeps the CPU rasterizer). */
int vc_pgl3d_init(void);

/* Non-zero once vc_pgl3d_init() succeeded and the 3D path is usable. */
int vc_pgl3d_probe(void);

/* Full-frame clear of the w x h render target backed by fb_phy/contig to the
 * packed rgba8 colour, resolving every tile to DRAM.  Returns a tri-state. */
int vc_pgl3d_clear(ewokos_addr_t fb_phy, uint8_t contig, uint32_t stride,
                   uint32_t w, uint32_t h, uint32_t fmt, uint32_t rgba8,
                   int rb_swap);

/* Draw req->nverts primitives into the render target described by req.
 * Returns a tri-state; the caller falls back to the CPU on anything but
 * VC_PGL3D_DONE. */
int vc_pgl3d_draw(const vc_pgl3d_draw_req_t *req);

/* ------------------------------------------------------------------ */
/* Compiled-shader program upload (WS4).                                 */
/*                                                                       */
/* A compiled program (VS/FS QPU words + uniform streams + prog_data)     */
/* arrives from the client as a serialized g2d PGL_PROGRAM blob, which      */
/* bsp_g3d parses into a vc_pgl3d_program_req_t.  vc_pgl3d_program          */
/* probe-gates it and forwards to the private v3d_3d_program, which uploads */
/* the QPU into GPU dma and caches the prog_data by handle.  A later draw    */
/* references the returned handle (WS4 draw-ref).  The request struct        */
/* mirrors the private v3d_3d_prog_desc_t field-for-field so libvideocore's  */
/* private v3d_3d.h never leaks into bsp_g3d (the same boundary the          */
/* vc_pgl3d_draw_req_t -> v3d_3d_draw translation already keeps).            */
/* ------------------------------------------------------------------ */

/* Program-upload result codes.  A non-negative return is the program handle
 * forwarded to the client; the two negatives are value-identical to the
 * G2D_ERR_FAILED / G2D_ERR_NOT_SUPPORTED codes so bsp_g3d forwards them
 * verbatim. */
#define VC_PGL3D_PROG_FAILED      (-1)
#define VC_PGL3D_PROG_UNSUPPORTED (-2)

/* Attribute / input slot caps, mirrored from the private v3d_3d layer so
 * bsp_g3d can pre-validate a parsed blob before crossing into libvideocore. */
#define VC_PGL3D_PROG_MAX_ATTRS  64u
#define VC_PGL3D_PROG_MAX_INPUTS 64u

/* One compiled-program upload request, filled by bsp_g3d from the parsed g2d
 * PGL_PROGRAM blob.  Every pointer is CPU-side (into the attached blob canvas)
 * and need only stay valid for the call: v3d_3d_program deep-copies the QPU
 * words + uniform value streams into GPU dma and the layout descriptors into
 * its inline cache. */
typedef struct {
    /* vertex stage */
    const uint32_t *vs_qpu;             uint32_t vs_qpu_words;
    const uint32_t *vs_unif_contents;   /* per-slot layout descriptor (CPU)  */
    const uint32_t *vs_unif_data;       uint32_t vs_num_uniforms;
    uint8_t  vs_threads;
    uint8_t  vs_single_seg;
    uint32_t vs_vpm_input_size;
    uint32_t vs_vpm_output_size;
    uint32_t vs_vcm_cache_size;
    uint8_t  vs_separate_segments;
    uint8_t  vs_vattr_sizes[VC_PGL3D_PROG_MAX_ATTRS / 4];
    int32_t  vs_driver_location_map[VC_PGL3D_PROG_MAX_ATTRS];
    /* fragment stage */
    const uint32_t *fs_qpu;             uint32_t fs_qpu_words;
    const uint32_t *fs_unif_contents;
    const uint32_t *fs_unif_data;       uint32_t fs_num_uniforms;
    uint8_t  fs_threads;
    uint8_t  fs_single_seg;
    uint8_t  fs_needs_coords;
    uint8_t  fs_uses_center_w;
    uint8_t  fs_writes_z;
    uint8_t  fs_disable_ez;
    uint32_t fs_num_inputs;
    uint32_t fs_input_slots[VC_PGL3D_PROG_MAX_INPUTS];
    /* coordinate (binning) stage: the same VS NIR compiled with is_coord=true.
     * Carries its own QPU + uniform stream and VPM layout (it writes only
     * gl_Position, no varyings, so its VPM output size differs from the render
     * VS's).  Shares the VS's uniform driver_locations, so one per-draw blob
     * feeds both expansions. */
    const uint32_t *coord_qpu;          uint32_t coord_qpu_words;
    const uint32_t *coord_unif_contents;
    const uint32_t *coord_unif_data;    uint32_t coord_num_uniforms;
    uint8_t  coord_threads;
    uint8_t  coord_single_seg;
    uint32_t coord_vpm_input_size;
    uint32_t coord_vpm_output_size;
    uint32_t coord_vcm_cache_size;
    uint8_t  coord_separate_segments;
    /* optional compute stage (g2d blob VERSION 3): the fragment program as a
     * CSD kernel writing the render target through SSBO binding 0.
     * cs_qpu_words == 0 means absent (the program draws through the binner). */
    const uint32_t *cs_qpu;             uint32_t cs_qpu_words;
    const uint32_t *cs_unif_contents;
    const uint32_t *cs_unif_data;       uint32_t cs_num_uniforms;
    uint8_t  cs_threads;
    uint8_t  cs_single_seg;
    uint32_t cs_local_size[3];
    uint32_t cs_shared_size;
    /* 1 = cs is the fixed geometry rasterizer (grid = one workgroup per
     * triangle; draw pos/col/z iovas become SSBO bindings 1/2/3); 0 = the
     * shadertoy full-frame route (grid covers the frame, SSBO 0 = fb). */
    uint32_t cs_is_geom;
} vc_pgl3d_program_req_t;

/* Upload a compiled program into GPU dma and cache it by handle.  Returns the
 * non-negative program handle on success, VC_PGL3D_PROG_UNSUPPORTED when the
 * 3D path is not ready or a payload is out of range, VC_PGL3D_PROG_FAILED on a
 * dma allocation failure.  The handle is later referenced by a draw request. */
int vc_pgl3d_program(const vc_pgl3d_program_req_t *req);

/* Drop a program uploaded by vc_pgl3d_program (slot + GPU dma released).
 * Returns 0, or VC_PGL3D_PROG_UNSUPPORTED for an unknown / free handle. */
int vc_pgl3d_program_free(int handle);

#endif /* VIDEOCORE_VC_PGL3D_H */
