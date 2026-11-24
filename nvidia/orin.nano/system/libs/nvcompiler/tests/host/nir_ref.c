/*
 * nir_ref.c - host-only NIR reference interpreter.  See nir_ref.h.
 * Float ops the backend expands (sin/cos in turns, div as rcp*mul, ...) are
 * evaluated with the same expressions as the SASS emulator (nv_ref_math.h), so
 * the two agree bit for bit when code generation is right.
 * SPDX-License-Identifier: MIT
 */
#pragma STDC FP_CONTRACT OFF
#include "nir_ref.h"
#include "nv_ref_math.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "compiler/nir/nir_constant_expressions.h"

enum { FLOW_NEXT, FLOW_BREAK, FLOW_CONTINUE, FLOW_RETURN, FLOW_ERROR };

struct nir_ref {
    nir_function_impl* impl;
    struct nir_ref_env env;
    nir_const_value* val;          /* ssa index * NIR_MAX_VEC_COMPONENTS */
    nir_block* prev;
    uint32_t wg[3], lid[3];
    char* err;
    size_t errsz;
};

static int rfail(struct nir_ref* r, const char* fmt, ...) {
    if (r->err) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(r->err, r->errsz, fmt, ap);
        va_end(ap);
    }
    return FLOW_ERROR;
}

static nir_const_value* V(struct nir_ref* r, nir_def* d) {
    return r->val + (size_t)d->index * NIR_MAX_VEC_COMPONENTS;
}

static bool as_bool(nir_def* d, nir_const_value v) {
    return d->bit_size == 1 ? v.b : v.u32 != 0;
}

/* scalar float32 ops evaluated the way the generated code computes them */
static bool float_override(nir_op op, const nir_const_value* a,
                           const nir_const_value* b, const nir_const_value* c,
                           nir_const_value* out) {
    float x = a ? a->f32 : 0.0f, y = b ? b->f32 : 0.0f, z = c ? c->f32 : 0.0f;
    float k = ref_u2f(REF_INV_2PI_BITS);
    memset(out, 0, sizeof(*out));
    switch (op) {
    case nir_op_fadd: out->f32 = x + y; return true;
    case nir_op_fsub: out->f32 = x + (-y); return true;
    case nir_op_fmul: out->f32 = x * y; return true;
    case nir_op_ffma: out->f32 = fmaf(x, y, z); return true;
    case nir_op_fneg: out->f32 = (-x) + (-0.0f); return true;
    case nir_op_fabs: out->f32 = fabsf(x) + (-0.0f); return true;
    case nir_op_fsat: out->f32 = ref_fsat(x + (-0.0f)); return true;
    case nir_op_fmin: out->f32 = ref_fmin(x, y); return true;
    case nir_op_fmax: out->f32 = ref_fmax(x, y); return true;
    case nir_op_fsin: out->f32 = ref_sin_turns(x * k); return true;
    case nir_op_fcos: out->f32 = ref_cos_turns(x * k); return true;
    case nir_op_fsin_normalized_2_pi: out->f32 = ref_sin_turns(x); return true;
    case nir_op_fcos_normalized_2_pi: out->f32 = ref_cos_turns(x); return true;
    case nir_op_fexp2: out->f32 = exp2f(x); return true;
    case nir_op_flog2: out->f32 = log2f(x); return true;
    case nir_op_frcp: out->f32 = ref_frcp(x); return true;
    case nir_op_frsq: out->f32 = ref_frsq(x); return true;
    case nir_op_fsqrt: out->f32 = sqrtf(x); return true;
    case nir_op_fdiv: out->f32 = ref_frcp(y) * x; return true;
    case nir_op_fpow: out->f32 = exp2f(log2f(x) * y); return true;
    case nir_op_ffloor: out->f32 = ref_frnd(x, 1); return true;
    case nir_op_fceil: out->f32 = ref_frnd(x, 2); return true;
    case nir_op_ftrunc: out->f32 = ref_frnd(x, 3); return true;
    case nir_op_fround_even: out->f32 = ref_frnd(x, 0); return true;
    case nir_op_ffract: out->f32 = x + (-ref_frnd(x, 1)); return true;
    case nir_op_f2i32: out->i32 = ref_f2i32(ref_frnd(x, 3)); return true;
    case nir_op_f2u32: out->u32 = ref_f2u32(ref_frnd(x, 3)); return true;
    case nir_op_i2f32: out->f32 = (float)a->i32; return true;
    case nir_op_u2f32: out->f32 = (float)a->u32; return true;
    default: return false;
    }
}

static int run_alu(struct nir_ref* r, nir_alu_instr* alu) {
    const nir_op_info* info = &nir_op_infos[alu->op];
    nir_const_value src[4][NIR_MAX_VEC_COMPONENTS];
    nir_const_value* srcp[4];
    unsigned nc = alu->def.num_components;
    if (info->num_inputs > 4)
        return rfail(r, "alu %s: too many inputs", info->name);
    bool per_comp = true;
    for (unsigned i = 0; i < info->num_inputs; i++) {
        unsigned n = info->input_sizes[i] ? info->input_sizes[i] : nc;
        if (info->input_sizes[i]) per_comp = false;
        nir_const_value* sv = V(r, alu->src[i].src.ssa);
        for (unsigned j = 0; j < n; j++)
            src[i][j] = sv[alu->src[i].swizzle[j]];
        srcp[i] = src[i];
    }
    nir_const_value out[NIR_MAX_VEC_COMPONENTS];
    memset(out, 0, sizeof(out));
    bool done = false;
    if (per_comp && info->output_size == 0 && info->num_inputs > 0 &&
        alu->def.bit_size == 32 && alu->src[0].src.ssa->bit_size == 32) {
        done = true;
        for (unsigned j = 0; j < nc && done; j++)
            done = float_override(alu->op,
                                  info->num_inputs > 0 ? &src[0][j] : NULL,
                                  info->num_inputs > 1 ? &src[1][j] : NULL,
                                  info->num_inputs > 2 ? &src[2][j] : NULL,
                                  &out[j]);
    }
    if (!done)
        nir_eval_const_opcode(alu->op, out, NULL, nc, alu->def.bit_size, srcp, 0);
    memcpy(V(r, &alu->def), out, nc * sizeof(nir_const_value));
    return FLOW_NEXT;
}

static int run_intrinsic(struct nir_ref* r, nir_intrinsic_instr* intr) {
    nir_const_value* dv = nir_intrinsic_infos[intr->intrinsic].has_dest
                          ? V(r, &intr->def) : NULL;
    switch (intr->intrinsic) {
    case nir_intrinsic_load_workgroup_id:
        for (int i = 0; i < 3; i++) dv[i].u64 = r->wg[i];
        return FLOW_NEXT;
    case nir_intrinsic_load_local_invocation_id:
        for (int i = 0; i < 3; i++) dv[i].u64 = r->lid[i];
        return FLOW_NEXT;
    case nir_intrinsic_load_base_workgroup_id:
    case nir_intrinsic_load_base_global_invocation_id:
        for (unsigned i = 0; i < intr->def.num_components; i++) dv[i].u64 = 0;
        return FLOW_NEXT;
    case nir_intrinsic_load_num_workgroups:
        for (int i = 0; i < 3; i++) dv[i].u64 = r->env.num_workgroups[i];
        return FLOW_NEXT;
    case nir_intrinsic_load_uniform: {
        uint32_t off = (uint32_t)nir_intrinsic_base(intr) +
                       V(r, intr->src[0].ssa)[0].u32;
        for (unsigned i = 0; i < intr->def.num_components; i++) {
            if (off + 4 * i + 4 > r->env.uniform_bytes)
                return rfail(r, "load_uniform past blob end (%u)", off + 4 * i);
            dv[i].u64 = 0;
            memcpy(&dv[i].u32, r->env.uniforms + off + 4 * i, 4);
        }
        return FLOW_NEXT;
    }
    case nir_intrinsic_load_ssbo:
    case nir_intrinsic_store_ssbo: {
        bool st = intr->intrinsic == nir_intrinsic_store_ssbo;
        uint32_t blk = V(r, intr->src[st ? 1 : 0].ssa)[0].u32;
        uint32_t off = V(r, intr->src[st ? 2 : 1].ssa)[0].u32;
        if (blk >= NIR_REF_MAX_SSBO || !r->env.ssbo[blk].p)
            return rfail(r, "ssbo block %u not bound", blk);
        unsigned n = st ? intr->src[0].ssa->num_components
                        : intr->def.num_components;
        unsigned mask = st ? nir_intrinsic_write_mask(intr) : ~0u;
        for (unsigned i = 0; i < n; i++) {
            if (!(mask & (1u << i)))
                continue;
            if ((uint64_t)off + 4 * i + 4 > r->env.ssbo[blk].size)
                return rfail(r, "ssbo %u access past end (offset %u)", blk,
                             off + 4 * i);
            uint8_t* p = r->env.ssbo[blk].p + off + 4 * i;
            if (st) {
                memcpy(p, &V(r, intr->src[0].ssa)[i].u32, 4);
            } else {
                dv[i].u64 = 0;
                memcpy(&dv[i].u32, p, 4);
            }
        }
        return FLOW_NEXT;
    }
    default:
        return rfail(r, "unsupported intrinsic %s",
                     nir_intrinsic_infos[intr->intrinsic].name);
    }
}

static int run_list(struct nir_ref* r, struct exec_list* list);

static int run_block(struct nir_ref* r, nir_block* block) {
    /* phis read their sources in parallel */
    nir_const_value tmp[64][NIR_MAX_VEC_COMPONENTS];
    unsigned np = 0;
    nir_foreach_phi(phi, block) {
        if (np == 64)
            return rfail(r, "too many phis in one block");
        bool found = false;
        nir_foreach_phi_src(ps, phi) {
            if (ps->pred != r->prev)
                continue;
            memcpy(tmp[np], V(r, ps->src.ssa), sizeof(tmp[np]));
            found = true;
        }
        if (!found)
            return rfail(r, "phi has no source for the executed predecessor");
        np++;
    }
    np = 0;
    nir_foreach_phi(phi, block)
        memcpy(V(r, &phi->def), tmp[np++], sizeof(tmp[0]));

    int flow = FLOW_NEXT;
    nir_foreach_instr(instr, block) {
        switch (instr->type) {
        case nir_instr_type_phi:
            break;
        case nir_instr_type_load_const: {
            nir_load_const_instr* lc = nir_instr_as_load_const(instr);
            memcpy(V(r, &lc->def), lc->value,
                   lc->def.num_components * sizeof(nir_const_value));
            break;
        }
        case nir_instr_type_undef:
            memset(V(r, &nir_instr_as_undef(instr)->def), 0,
                   NIR_MAX_VEC_COMPONENTS * sizeof(nir_const_value));
            break;
        case nir_instr_type_alu:
            flow = run_alu(r, nir_instr_as_alu(instr));
            break;
        case nir_instr_type_intrinsic:
            flow = run_intrinsic(r, nir_instr_as_intrinsic(instr));
            break;
        case nir_instr_type_jump:
            switch (nir_instr_as_jump(instr)->type) {
            case nir_jump_break: flow = FLOW_BREAK; break;
            case nir_jump_continue: flow = FLOW_CONTINUE; break;
            case nir_jump_return:
            case nir_jump_halt: flow = FLOW_RETURN; break;
            default: flow = rfail(r, "unsupported jump"); break;
            }
            break;
        default:
            flow = rfail(r, "unsupported instruction type %d", instr->type);
            break;
        }
        if (flow != FLOW_NEXT)
            break;
    }
    r->prev = block;
    return flow;
}

static int run_list(struct nir_ref* r, struct exec_list* list) {
    foreach_list_typed(nir_cf_node, node, node, list) {
        int flow = FLOW_NEXT;
        switch (node->type) {
        case nir_cf_node_block:
            flow = run_block(r, nir_cf_node_as_block(node));
            break;
        case nir_cf_node_if: {
            nir_if* nif = nir_cf_node_as_if(node);
            nir_def* cd = nif->condition.ssa;
            flow = run_list(r, as_bool(cd, V(r, cd)[0]) ? &nif->then_list
                                                         : &nif->else_list);
            break;
        }
        case nir_cf_node_loop: {
            nir_loop* loop = nir_cf_node_as_loop(node);
            for (uint64_t iter = 0;; iter++) {
                if (iter > 100000000ull)
                    return rfail(r, "runaway loop");
                flow = run_list(r, &loop->body);
                if (flow == FLOW_NEXT || flow == FLOW_CONTINUE) {
                    flow = FLOW_NEXT;
                    if (nir_loop_has_continue_construct(loop))
                        flow = run_list(r, &loop->continue_list);
                }
                if (flow != FLOW_NEXT)
                    break;
            }
            if (flow == FLOW_BREAK)
                flow = FLOW_NEXT;
            break;
        }
        default:
            return rfail(r, "unsupported cf node");
        }
        if (flow != FLOW_NEXT)
            return flow;
    }
    return FLOW_NEXT;
}

struct nir_ref* nir_ref_create(nir_shader* nir, const struct nir_ref_env* env) {
    struct nir_ref* r = calloc(1, sizeof(*r));
    r->impl = nir_shader_get_entrypoint(nir);
    r->env = *env;
    nir_index_ssa_defs(r->impl);
    r->val = calloc((size_t)r->impl->ssa_alloc * NIR_MAX_VEC_COMPONENTS + 1,
                    sizeof(nir_const_value));
    return r;
}

void nir_ref_destroy(struct nir_ref* r) {
    if (!r) return;
    free(r->val);
    free(r);
}

int nir_ref_run(struct nir_ref* r, const uint32_t workgroup[3],
                const uint32_t local[3], char* err, size_t errsz) {
    r->err = err;
    r->errsz = errsz;
    r->prev = NULL;
    memcpy(r->wg, workgroup, sizeof(r->wg));
    memcpy(r->lid, local, sizeof(r->lid));
    return run_list(r, &r->impl->body) == FLOW_ERROR ? -1 : 0;
}
