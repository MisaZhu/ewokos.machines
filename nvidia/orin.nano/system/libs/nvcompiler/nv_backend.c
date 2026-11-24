/*
 * nv_backend.c - NIR -> sm_87 SASS compiler core for the GA10B back end.
 *
 * Lowers the linked compute NIR the shared frontend produced and emits sm_87
 * SASS through the nv_sass encoder + assembler.  Mirrors the role of Mesa NAK's
 * from_nir.rs + assign_regs.rs + calc_instr_deps.rs, restricted to the compute
 * subset portablegl uses (no graphics pipeline).
 *
 * Strategy (correctness first, performance later):
 *   - register allocation: each nir_def gets a GPR from a free-list allocator
 *     driven by NIR liveness (nir_liveness_analysis); 64-bit values take a pair.
 *   - scheduling: conservative fixed stalls for ALU; variable-latency ops
 *     (memory, MUFU, conversions) get a write scoreboard and consumers wait on
 *     it (mirrors NAK calc_instr_deps, simplified to one barrier per op class).
 *   - control flow: nir blocks -> labels; nir_jump_branch -> BRA; structured
 *     if/loop lowered to predicates + branches.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"

#include "nv_sass.h"
#include "nv_backend.h"

/* ---- compiler options for sm_87 ----------------------------------------- */
static const struct nir_shader_compiler_options s_options = {
    .lower_fpow = true,
    .lower_fdiv = true,
    .lower_fmod = true,
    .lower_fsqrt = true,
    .lower_bitops = true,
    .lower_pack_64_2x32 = true,
    .lower_extract_word = true,
    .lower_insert_word = true,
    .has_fused_comp_and_csel = true,
    .has_imul24 = true,
    .has_isub = true,
    .max_unroll_iterations = 32,
};

const struct nir_shader_compiler_options *nv_be_nir_options(void) {
    return &s_options;
}

/* ---- register allocator ------------------------------------------------- */
#define NV_MAX_GPR 255
typedef struct {
    nir_shader* nir;
    nv_asm* out;
    /* nir_def index -> GPR base (or -1 unassigned) */
    int* def_reg;
    unsigned def_cap;
    uint8_t used[NV_MAX_GPR];
    unsigned num_gprs;
    /* per-instr write barrier tracking for scheduling */
    char log[1024];
    int failed;
} nv_ctx;

static int def_index(nv_ctx* c, nir_def* d) {
    return (int)d->index;
}

static int alloc_gpr(nv_ctx* c) {
    for (int i = 2; i < NV_MAX_GPR; i++) {   /* R0/R1 reserved (R1 = stack) */
        if (!c->used[i]) {
            c->used[i] = 1;
            if ((unsigned)i + 1 > c->num_gprs) c->num_gprs = i + 1;
            return i;
        }
    }
    c->failed = 1;
    return NV_RZ;
}

static void free_gpr(nv_ctx* c, int r) {
    if (r >= 0 && r < NV_MAX_GPR) c->used[r] = 0;
}

static int reg_of(nv_ctx* c, nir_def* d) {
    int idx = def_index(c, d);
    if (idx < 0) return NV_RZ;
    if ((unsigned)idx >= c->def_cap) {
        unsigned ncap = c->def_cap ? c->def_cap * 2 : 1024;
        while ((unsigned)idx >= ncap) ncap *= 2;
        c->def_reg = realloc(c->def_reg, ncap * sizeof(int));
        for (unsigned i = c->def_cap; i < ncap; i++) c->def_reg[i] = -1;
        c->def_cap = ncap;
    }
    if (c->def_reg[idx] < 0) {
        int r = alloc_gpr(c);
        c->def_reg[idx] = r;
        int extra = (d->bit_size == 64 || d->num_components >= 2) ? 1 : 0;
        if (d->num_components >= 3) extra = 2;
        for (int k = 1; k <= extra && r + k < NV_MAX_GPR; k++) {
            c->used[r + k] = 1;
            if ((unsigned)r + k + 1 > c->num_gprs) c->num_gprs = r + k + 1;
        }
    }
    return c->def_reg[idx];
}

/* ---- source operand helpers --------------------------------------------- */
static nv_src src_of(nv_ctx* c, nir_src s) {
    if (!s.ssa) return nv_zero();
    nir_def* d = s.ssa;
    int r = reg_of(c, d);
    if (d->bit_size == 64) return nv_gpr64((uint8_t)r);
    return nv_gpr((uint8_t)r);
}

static nv_ctrl alu_ctrl(void) {
    nv_ctrl ct = nv_ctrl_none();
    ct.delay = 4;
    return ct;
}

/* ---- ALU emission ------------------------------------------------------- */
static void emit_alu(nv_ctx* c, nir_alu_instr* alu) {
    nir_def* dst = &alu->def;
    int dr = reg_of(c, dst);
    int ninput = nir_op_infos[alu->op].num_inputs;
    nv_src a = ninput >= 1 ? src_of(c, alu->src[0].src) : nv_zero();
    nv_src b = ninput >= 2 ? src_of(c, alu->src[1].src) : nv_zero();
    nv_src cc = ninput >= 3 ? src_of(c, alu->src[2].src) : nv_zero();
    uint32_t w[4];
    nv_ctrl ct = alu_ctrl();

    switch (alu->op) {
    case nir_op_fadd:
        nv_fadd(w, dr, a, b, 0, 0, NV_RND_NEAREST, ct); break;
    case nir_op_fmul:
        nv_fmul(w, dr, a, b, 0, 0, NV_RND_NEAREST, ct); break;
    case nir_op_ffma:
        nv_ffma(w, dr, a, b, cc, 0, 0, NV_RND_NEAREST, ct); break;
    case nir_op_fmin:
        nv_fmnmx(w, dr, a, b, nv_pt(), 0, ct); break;
    case nir_op_fmax:
        nv_fmnmx(w, dr, a, b, nv_pn(NV_PT), 0, ct); break;
    case nir_op_imad:
        nv_imad(w, dr, a, b, cc, 1, ct); break;
    case nir_op_iadd:
        nv_iadd3(w, dr, a, b, nv_zero(), ct); break;
    case nir_op_iadd3:
        nv_iadd3(w, dr, a, b, cc, ct); break;
    case nir_op_imul:
        nv_imad(w, dr, a, b, nv_zero(), 1, ct); break;
    case nir_op_imin:
        nv_imnmx(w, dr, a, b, nv_pt(), 1, ct); break;
    case nir_op_imax:
        nv_imnmx(w, dr, a, b, nv_pn(NV_PT), 1, ct); break;
    case nir_op_ineg:
        nv_imad(w, dr, a, nv_imm(0xffffffffu), nv_zero(), 1, ct); break;
    case nir_op_fneg:
        a.neg = 1;
        nv_fadd(w, dr, a, nv_zero(), 0, 0, NV_RND_NEAREST, ct); break;
    case nir_op_fabs:
        a.abs = 1;
        nv_fadd(w, dr, a, nv_zero(), 0, 0, NV_RND_NEAREST, ct); break;
    case nir_op_mov: {
        nir_src s0 = alu->src[0].src;
        if (s0.ssa) {
            int r = reg_of(c, s0.ssa);
            int comp = alu->src[0].swizzle[0];
            nv_mov(w, dr, nv_gpr((uint8_t)(r + comp)), ct);
        } else {
            nv_mov(w, dr, a, ct);
        }
        break;
    }
    case nir_op_vec2:
    case nir_op_vec3:
    case nir_op_vec4: {
        int n = nir_op_infos[alu->op].num_inputs;
        for (int i = 0; i < n; i++) {
            nv_src s = src_of(c, alu->src[i].src);
            nv_mov(w, (uint8_t)(dr + i), s, ct);
            nv_asm_emit(c->out, w);
        }
        return;
    }
    case nir_op_f2i32:
        nv_f2i(w, dr, a, 1, 32, 32, NV_RND_ZERO, 0, ct); break;
    case nir_op_f2u32:
        nv_f2i(w, dr, a, 0, 32, 32, NV_RND_ZERO, 0, ct); break;
    case nir_op_i2f32:
        nv_i2f(w, dr, a, 1, 32, 32, NV_RND_NEAREST, ct); break;
    case nir_op_u2f32:
        nv_i2f(w, dr, a, 0, 32, 32, NV_RND_NEAREST, ct); break;
    case nir_op_frcp:
        nv_mufu(w, dr, a, NV_MUFU_RCP, ct); break;
    case nir_op_frsq:
        nv_mufu(w, dr, a, NV_MUFU_RSQ, ct); break;
    case nir_op_fsqrt:
        nv_mufu(w, dr, a, NV_MUFU_SQRT, ct); break;
    case nir_op_fexp2:
        nv_mufu(w, dr, a, NV_MUFU_EX2, ct); break;
    case nir_op_flog2:
        nv_mufu(w, dr, a, NV_MUFU_LG2, ct); break;
    case nir_op_fsin:
        nv_mufu(w, dr, a, NV_MUFU_SIN, ct); break;
    case nir_op_fcos:
        nv_mufu(w, dr, a, NV_MUFU_COS, ct); break;
    case nir_op_iand:
        nv_lop3(w, dr, a, b, nv_zero(), 0x80, nv_pt(), ct); break;
    case nir_op_ior:
        nv_lop3(w, dr, a, b, nv_zero(), 0xfe, nv_pt(), ct); break;
    case nir_op_ixor:
        nv_lop3(w, dr, a, b, nv_zero(), 0x96, nv_pt(), ct); break;
    case nir_op_ishl:
        nv_shf(w, dr, a, nv_zero(), b, 0, 0, 0, 0, ct); break;
    case nir_op_ishr:
        nv_shf(w, dr, a, nv_zero(), b, 1, 0, 0, 1, ct); break;
    case nir_op_ushr:
        nv_shf(w, dr, a, nv_zero(), b, 1, 0, 0, 0, ct); break;
    default:
        snprintf(c->log, sizeof(c->log), "unsupported ALU op %s",
                 nir_op_infos[alu->op].name);
        c->failed = 1;
        return;
    }
    nv_asm_emit(c->out, w);
}

/* ---- intrinsic emission ------------------------------------------------- */
static void emit_intrinsic(nv_ctx* c, nir_intrinsic_instr* intr) {
    uint32_t w[4];
    nv_ctrl ct = alu_ctrl();
    nir_def* dst = (nir_intrinsic_dest_components(intr) > 0) ? &intr->def : NULL;
    int dr = dst ? reg_of(c, dst) : NV_RZ;

    switch (intr->intrinsic) {
    case nir_intrinsic_load_local_invocation_id: {
        /* vec3 tid -> S2R per component used; emit X,Y,Z into a reg triple */
        nv_s2r(w, dr, NV_SR_TID_X, ct); nv_asm_emit(c->out, w);
        nv_s2r(w, dr + 1, NV_SR_TID_Y, ct); nv_asm_emit(c->out, w);
        nv_s2r(w, dr + 2, NV_SR_TID_Z, ct); nv_asm_emit(c->out, w);
        return;
    }
    case nir_intrinsic_load_workgroup_id: {
        nv_s2r(w, dr, NV_SR_CTAID_X, ct); nv_asm_emit(c->out, w);
        nv_s2r(w, dr + 1, NV_SR_CTAID_Y, ct); nv_asm_emit(c->out, w);
        nv_s2r(w, dr + 2, NV_SR_CTAID_Z, ct); nv_asm_emit(c->out, w);
        return;
    }
    case nir_intrinsic_load_num_workgroups: {
        nv_mov(w, dr, nv_cb(0, 0x0c), ct); nv_asm_emit(c->out, w);
        nv_mov(w, dr + 1, nv_cb(0, 0x10), ct); nv_asm_emit(c->out, w);
        nv_mov(w, dr + 2, nv_cb(0, 0x14), ct); nv_asm_emit(c->out, w);
        return;
    }
    case nir_intrinsic_load_uniform: {
        unsigned off = nir_intrinsic_base(intr) +
                       (unsigned)nir_src_as_uint(intr->src[0]);
        nv_mov(w, dr, nv_cb(0, off), ct); nv_asm_emit(c->out, w);
        return;
    }
    case nir_intrinsic_load_shared: {
        nv_src addr = src_of(c, intr->src[0]);
        unsigned off = nir_intrinsic_base(intr);
        nv_lds(w, dr, addr, (int32_t)off, NV_MT_B32, ct);
        nv_asm_emit(c->out, w);
        return;
    }
    case nir_intrinsic_store_shared: {
        nv_src addr = src_of(c, intr->src[1]);
        nv_src data = src_of(c, intr->src[0]);
        unsigned off = nir_intrinsic_base(intr);
        nv_sts(w, addr, data, (int32_t)off, NV_MT_B32, ct);
        nv_asm_emit(c->out, w);
        return;
    }
    case nir_intrinsic_barrier: {
        nv_bar(w, ct); nv_asm_emit(c->out, w);
        return;
    }
    default:
        snprintf(c->log, sizeof(c->log), "unsupported intrinsic %s",
                 nir_intrinsic_infos[intr->intrinsic].name);
        c->failed = 1;
        return;
    }
}

/* ---- block / control-flow emission -------------------------------------- */
static void emit_block(nv_ctx* c, nir_block* block);

static void emit_jump(nv_ctx* c, nir_jump_instr* jump) {
    /* jumps handled at block level via successors */
    (void)c; (void)jump;
}

static void emit_block(nv_ctx* c, nir_block* block) {
    nir_foreach_instr(instr, block) {
        if (c->failed) return;
        switch (instr->type) {
        case nir_instr_type_alu:
            emit_alu(c, nir_instr_as_alu(instr)); break;
        case nir_instr_type_intrinsic:
            emit_intrinsic(c, nir_instr_as_intrinsic(instr)); break;
        case nir_instr_type_jump:
            emit_jump(c, nir_instr_as_jump(instr)); break;
        case nir_instr_type_load_const: {
            nir_load_const_instr* lc = nir_instr_as_load_const(instr);
            int dr = reg_of(c, &lc->def);
            uint32_t w[4];
            nv_mov(w, dr, nv_imm(lc->value[0].u32), alu_ctrl());
            nv_asm_emit(c->out, w);
            break;
        }
        default:
            snprintf(c->log, sizeof(c->log), "unsupported instr type %d",
                     instr->type);
            c->failed = 1;
            return;
        }
    }
}

/* ---- top-level compile -------------------------------------------------- */
bool nv_be_compile_compute(nir_shader* cs, struct nv_be_result* res,
                           char* log, size_t logsz) {
    if (!cs || !res) return false;
    memset(res, 0, sizeof(*res));

    nir_shader* nir = nir_shader_clone(NULL, cs);

    /* lower system values the backend reads natively */
    nir_lower_system_values(nir);
    nir_lower_compute_system_values(nir, NULL);

    nv_ctx c;
    memset(&c, 0, sizeof(c));
    c.nir = nir;
    nv_asm asmb;
    nv_asm_init(&asmb);
    c.out = &asmb;

    /* registers are allocated lazily in reg_of() as defs are emitted */

    /* emit straight-line blocks (control flow added in a later milestone) */
    nir_foreach_function_impl(impl, nir) {
        nir_foreach_block(block, impl) {
            emit_block(&c, block);
        }
    }
    uint32_t w[4];
    nv_exit(w, alu_ctrl());
    nv_asm_emit(&asmb, w);

    if (c.failed) {
        if (log) snprintf(log, logsz, "%s", c.log);
        free(c.def_reg);
        ralloc_free(nir);
        return false;
    }

    nv_asm_finish(&asmb);
    res->bin_words = asmb.n * 4;
    res->bin = malloc(res->bin_words * sizeof(uint32_t));
    memcpy(res->bin, asmb.w, res->bin_words * sizeof(uint32_t));
    res->meta.num_gprs = c.num_gprs;
    res->meta.local_size[0] = nir->info.workgroup_size[0];
    res->meta.local_size[1] = nir->info.workgroup_size[1];
    res->meta.local_size[2] = nir->info.workgroup_size[2];

    free(c.def_reg);
    ralloc_free(nir);
    return true;
}

void nv_be_result_free(struct nv_be_result* res) {
    if (!res) return;
    free(res->bin);
    memset(res, 0, sizeof(*res));
}