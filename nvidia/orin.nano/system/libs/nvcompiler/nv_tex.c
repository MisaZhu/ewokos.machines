/*
 * nv_tex.c - software texture sampling for the GA10B compute path.
 *
 * GA10B compute kernels have no TMU port here, so a nir_tex_instr is expanded
 * into plain loads + ALU over the texture's linear RGBA-word image (the same
 * layout the precompiled geometry kernels index, nv_texlayout.c):
 *   - the image rides SSBO block tex_ssbo_base + texture_index (base 1 for
 *     the fullscreen compute shape where block 0 is the fb; the Vulkan
 *     geom passes set 2/3 via the pglc_tex_base source marker);
 *   - w / h / row stride / cube face stride / mag filter come from a per-unit
 *     descriptor table in constant bank 0 (nv_meta.h NV_TEXDESC_OFF), which
 *     the daemon fills from the draw's ctex bindings at launch;
 *   - 2D: GL_REPEAT wrap (the engine only stages REPEAT 2D textures) +
 *     bilinear; cube: the GL major-axis face selection (identical to
 *     portablegl's texture_cube_coords) + clamp-to-edge bilinear per face.
 * NEAREST degenerates the same bilinear to one texel (pos +0.5, weights 0).
 * Only level 0 is sampled; the daemon declines multi-level bindings.
 *
 * Runs on vector NIR before the backend's scalarization, so everything emitted
 * here goes through the existing ALU/intrinsic lowering unchanged.
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"

#include "nv_backend.h"

static nir_def* desc_load(nir_builder* b, uint32_t unit, unsigned dword) {
    return nir_load_uniform(b, 1, 32, nir_imm_int(b, 0),
                            .base = (int)(NV_TEXDESC_OFF - NV_UNIFORM_BASE_OFF +
                                          unit * NV_TEXDESC_STRIDE + dword * 4u),
                            .range = 4);
}

/* one texel: linear RGBA bytes in memory -> vec4 float in [0,1] */
static nir_def* fetch_texel(nir_builder* b, nir_def* block, nir_def* stride,
                            nir_def* base_words, nir_def* x, nir_def* y) {
    nir_def* row = nir_imul(b, y, stride);
    nir_def* idx = nir_iadd(b, nir_iadd(b, row, x), base_words);
    nir_def* addr = nir_ishl(b, idx, nir_imm_int(b, 2));
    nir_def* w = nir_load_ssbo(b, 1, 32, block, addr,
                               .access = ACCESS_NON_WRITEABLE, .align_mul = 4);
    nir_def* c[4];
    for (unsigned i = 0; i < 4; i++) {
        nir_def* byte = i ? nir_ushr(b, w, nir_imm_int(b, 8 * i)) : w;
        if (i < 3)
            byte = nir_iand(b, byte, nir_imm_int(b, 255));
        c[i] = nir_i2f32(b, byte);
    }
    return nir_fmul(b, nir_vec(b, c, 4), nir_imm_float(b, 1.0f / 255.0f));
}

static nir_def* mix4(nir_builder* b, nir_def* a, nir_def* b2, nir_def* f) {
    return nir_ffma(b, nir_fsub(b, b2, a), f, a);
}

/* sample a face at normalized (s,t); clamp to edge.  `half` shifts the sample
 * position for NEAREST, `linf` zeroes the interpolation weights. */
static nir_def* sample_face(nir_builder* b, nir_def* block,
                            nir_def* w_i, nir_def* h_i, nir_def* stride,
                            nir_def* base_words,
                            nir_def* s, nir_def* t,
                            nir_def* half, nir_def* linf) {
    nir_def* wf = nir_i2f32(b, w_i);
    nir_def* hf = nir_i2f32(b, h_i);
    nir_def* sx = nir_fadd(b, nir_ffma(b, s, wf, nir_imm_float(b, -0.5f)), half);
    nir_def* sy = nir_fadd(b, nir_ffma(b, t, hf, nir_imm_float(b, -0.5f)), half);
    nir_def* x0f = nir_ffloor(b, sx);
    nir_def* y0f = nir_ffloor(b, sy);
    nir_def* fx = nir_fmul(b, nir_fsub(b, sx, x0f), linf);
    nir_def* fy = nir_fmul(b, nir_fsub(b, sy, y0f), linf);
    nir_def* x0 = nir_f2i32(b, x0f);
    nir_def* y0 = nir_f2i32(b, y0f);
    nir_def* wm1 = nir_isub(b, w_i, nir_imm_int(b, 1));
    nir_def* hm1 = nir_isub(b, h_i, nir_imm_int(b, 1));
    nir_def* xa = nir_imax(b, nir_imin(b, x0, wm1), nir_imm_int(b, 0));
    nir_def* xb = nir_imax(b, nir_imin(b, nir_iadd(b, x0, nir_imm_int(b, 1)), wm1),
                           nir_imm_int(b, 0));
    nir_def* ya = nir_imax(b, nir_imin(b, y0, hm1), nir_imm_int(b, 0));
    nir_def* yb = nir_imax(b, nir_imin(b, nir_iadd(b, y0, nir_imm_int(b, 1)), hm1),
                           nir_imm_int(b, 0));
    nir_def* t00 = fetch_texel(b, block, stride, base_words, xa, ya);
    nir_def* t10 = fetch_texel(b, block, stride, base_words, xb, ya);
    nir_def* t01 = fetch_texel(b, block, stride, base_words, xa, yb);
    nir_def* t11 = fetch_texel(b, block, stride, base_words, xb, yb);
    return mix4(b, mix4(b, t00, t10, fx), mix4(b, t01, t11, fx), fy);
}

static bool lower_tex_instr(nir_builder* b, nir_tex_instr* tex,
                            uint32_t* tex_mask, uint32_t tex_ssbo_base,
                            char* log, size_t logsz) {
    if (tex->op != nir_texop_tex) {
        snprintf(log, logsz, "tex op %d unsupported (plain texture() only)",
                 tex->op);
        return false;
    }
    if (tex->is_array || tex->is_shadow ||
        tex->sampler_dim == GLSL_SAMPLER_DIM_3D ||
        nir_tex_instr_has_explicit_tg4_offsets(tex)) {
        snprintf(log, logsz, "tex array/shadow/3D/offsets unsupported");
        return false;
    }
    int ci = nir_tex_instr_src_index(tex, nir_tex_src_coord);
    if (ci < 0 || tex->coord_components < 2) {
        snprintf(log, logsz, "tex without coordinates");
        return false;
    }
    for (unsigned i = 0; i < tex->num_srcs; i++) {
        if (i == (unsigned)ci)
            continue;
        snprintf(log, logsz, "tex src %d unsupported (lod/bias/offset)",
                 tex->src[i].src_type);
        return false;
    }
    uint32_t unit = tex->texture_index;
    if (unit >= NV_MAX_TEX || unit + tex_ssbo_base >= NV_MAX_SSBO) {
        snprintf(log, logsz, "texture index %u exceeds %u units",
                 unit, NV_MAX_TEX);
        return false;
    }
    bool cube = tex->sampler_dim == GLSL_SAMPLER_DIM_CUBE;
    if (!cube && tex->sampler_dim != GLSL_SAMPLER_DIM_2D) {
        snprintf(log, logsz, "sampler dim %d unsupported (2D/cube only)",
                 tex->sampler_dim);
        return false;
    }

    b->cursor = nir_before_instr(&tex->instr);
    nir_def* coord = tex->src[ci].src.ssa;
    nir_def* block = nir_imm_int(b, (int)(unit + tex_ssbo_base));
    nir_def* w_i = desc_load(b, unit, 0);
    nir_def* h_i = desc_load(b, unit, 1);
    nir_def* stride = desc_load(b, unit, 2);
    nir_def* lin_i = desc_load(b, unit, 4);
    nir_def* linf = nir_i2f32(b, lin_i);
    /* NEAREST: sample at texel centers with zero weights */
    nir_def* half = nir_fmul(b, nir_fsub(b, nir_imm_float(b, 1.0f), linf),
                             nir_imm_float(b, 0.5f));
    nir_def* zero = nir_imm_int(b, 0);
    nir_def* result;

    if (!cube) {
        /* REPEAT in normalized space: fract() also wraps negatives and the
         * u==1.0 edge (x0 = -1 blends the last and first texel). */
        nir_def* u = nir_ffract(b, nir_channel(b, coord, 0));
        nir_def* v = nir_ffract(b, nir_channel(b, coord, 1));
        nir_def* wf = nir_i2f32(b, w_i);
        nir_def* hf = nir_i2f32(b, h_i);
        nir_def* sx = nir_fadd(b, nir_ffma(b, u, wf, nir_imm_float(b, -0.5f)), half);
        nir_def* sy = nir_fadd(b, nir_ffma(b, v, hf, nir_imm_float(b, -0.5f)), half);
        nir_def* x0f = nir_ffloor(b, sx);
        nir_def* y0f = nir_ffloor(b, sy);
        nir_def* fx = nir_fmul(b, nir_fsub(b, sx, x0f), linf);
        nir_def* fy = nir_fmul(b, nir_fsub(b, sy, y0f), linf);
        nir_def* x0 = nir_f2i32(b, x0f);
        nir_def* y0 = nir_f2i32(b, y0f);
        nir_def* x1 = nir_iadd(b, x0, nir_imm_int(b, 1));
        nir_def* y1 = nir_iadd(b, y0, nir_imm_int(b, 1));
        /* x0 in [-1,w-1], x1 in [0,w]: one correction each.  The final clamp
         * is a safety net for NaN/inf coordinates (undefined in GLSL, e.g.
         * the frontend folds normalize(0) to NaN): f2i of those is wild, and
         * an unclamped index would read outside the image buffer. */
        x0 = nir_bcsel(b, nir_ilt(b, x0, zero), nir_iadd(b, x0, w_i), x0);
        x1 = nir_bcsel(b, nir_ige(b, x1, w_i), nir_isub(b, x1, w_i), x1);
        y0 = nir_bcsel(b, nir_ilt(b, y0, zero), nir_iadd(b, y0, h_i), y0);
        y1 = nir_bcsel(b, nir_ige(b, y1, h_i), nir_isub(b, y1, h_i), y1);
        nir_def* wm1 = nir_isub(b, w_i, nir_imm_int(b, 1));
        nir_def* hm1 = nir_isub(b, h_i, nir_imm_int(b, 1));
        x0 = nir_imax(b, nir_imin(b, x0, wm1), zero);
        x1 = nir_imax(b, nir_imin(b, x1, wm1), zero);
        y0 = nir_imax(b, nir_imin(b, y0, hm1), zero);
        y1 = nir_imax(b, nir_imin(b, y1, hm1), zero);
        nir_def* t00 = fetch_texel(b, block, stride, zero, x0, y0);
        nir_def* t10 = fetch_texel(b, block, stride, zero, x1, y0);
        nir_def* t01 = fetch_texel(b, block, stride, zero, x0, y1);
        nir_def* t11 = fetch_texel(b, block, stride, zero, x1, y1);
        result = mix4(b, mix4(b, t00, t10, fx), mix4(b, t01, t11, fx), fy);
    } else {
        /* GL major-axis face selection, matching portablegl's
         * texture_cube_coords (X first on ties, faces +X,-X,+Y,-Y,+Z,-Z). */
        nir_def* x = nir_channel(b, coord, 0);
        nir_def* y = nir_channel(b, coord, 1);
        nir_def* z = nir_channel(b, coord, 2);
        nir_def* ax = nir_fabs(b, x);
        nir_def* ay = nir_fabs(b, y);
        nir_def* az = nir_fabs(b, z);
        nir_def* ma = nir_fmax(b, nir_fmax(b, ax, ay), az);
        nir_def* sel_x = nir_fge(b, ax, nir_fmax(b, ay, az));
        nir_def* sel_y = nir_fge(b, ay, az);
        nir_def* xn = nir_flt(b, x, nir_imm_float(b, 0.0f));
        nir_def* yn = nir_flt(b, y, nir_imm_float(b, 0.0f));
        nir_def* zn = nir_flt(b, z, nir_imm_float(b, 0.0f));
        nir_def* sc = nir_bcsel(b, sel_x,
                                nir_bcsel(b, xn, z, nir_fneg(b, z)),
                                nir_bcsel(b, sel_y, x,
                                          nir_bcsel(b, zn, nir_fneg(b, x), x)));
        nir_def* tc = nir_bcsel(b, sel_x, nir_fneg(b, y),
                                nir_bcsel(b, sel_y,
                                          nir_bcsel(b, yn, nir_fneg(b, z), z),
                                          nir_fneg(b, y)));
        nir_def* face = nir_bcsel(b, sel_x,
                                  nir_bcsel(b, xn, nir_imm_int(b, 1), zero),
                                  nir_bcsel(b, sel_y,
                                            nir_bcsel(b, yn, nir_imm_int(b, 3),
                                                      nir_imm_int(b, 2)),
                                            nir_bcsel(b, zn, nir_imm_int(b, 5),
                                                      nir_imm_int(b, 4))));
        nir_def* rma = nir_frcp(b, ma);
        nir_def* s = nir_fmul(b, nir_fadd(b, nir_fmul(b, sc, rma),
                                          nir_imm_float(b, 1.0f)),
                              nir_imm_float(b, 0.5f));
        nir_def* t = nir_fmul(b, nir_fadd(b, nir_fmul(b, tc, rma),
                                          nir_imm_float(b, 1.0f)),
                              nir_imm_float(b, 0.5f));
        /* the zero vector (e.g. refract() total internal reflection) samples
         * face 0 at (0,0), as the CPU interpreter does */
        nir_def* is_zero = nir_feq(b, ma, nir_imm_float(b, 0.0f));
        s = nir_bcsel(b, is_zero, nir_imm_float(b, 0.0f), s);
        t = nir_bcsel(b, is_zero, nir_imm_float(b, 0.0f), t);
        nir_def* fstride = desc_load(b, unit, 3);
        nir_def* base_words = nir_imul(b, face, fstride);
        result = sample_face(b, block, w_i, h_i, stride, base_words,
                             s, t, half, linf);
    }

    nir_def_rewrite_uses(&tex->def, result);
    nir_instr_remove(&tex->instr);
    *tex_mask |= 1u << unit;
    return true;
}

bool nv_lower_tex(nir_shader* nir, uint32_t* tex_mask, uint32_t tex_ssbo_base,
                  char* log, size_t logsz) {
    *tex_mask = 0;
    bool ok = true;
    nir_foreach_function_impl(impl, nir) {
        nir_builder b = nir_builder_create(impl);
        nir_foreach_block_safe(block, impl) {
            nir_foreach_instr_safe(instr, block) {
                if (instr->type != nir_instr_type_tex)
                    continue;
                if (!lower_tex_instr(&b, nir_instr_as_tex(instr), tex_mask,
                                     tex_ssbo_base, log, logsz)) {
                    ok = false;
                    break;
                }
            }
            if (!ok)
                break;
        }
        if (!ok)
            break;
        nir_progress(true, impl, nir_metadata_none);
    }
    return ok;
}