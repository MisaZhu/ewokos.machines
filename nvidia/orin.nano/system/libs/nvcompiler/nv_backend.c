/*
 * nv_backend.c - NIR -> sm_87 SASS compiler core for the GA10B back end.
 *
 * Lowers the linked compute NIR the shared frontend produced and emits sm_87
 * SASS through the nv_sass encoder + assembler.  Mirrors the role of Mesa NAK's
 * from_nir.rs + assign_regs.rs + calc_instr_deps.rs, restricted to the compute
 * subset portablegl uses (no graphics pipeline).
 *
 *   - lowering: ALU/phi/load_const are scalarized, so every value is a run of
 *     32-bit components and every ALU op is scalar; constants are operands.
 *   - registers: linear scan over the structured emission order.  The hardware
 *     owns the top two GPRs of the declared count, so R0..R252 are usable and
 *     the kernel declares (highest used + 1) + 2.
 *   - scheduling: serial, as NAK's assign_deps_serial: every instruction waits
 *     for its predecessor's write (scoreboard 0) and read (scoreboard 1).
 *   - control flow: every if/loop is a BCLEAR+BSSY ... BSYNC scope; break and
 *     continue leave through each enclosing BSYNC (nak_nir_lower_cf).
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdarg.h>
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

/* ---- context ------------------------------------------------------------ */
#define NV_GPR_LIMIT   253   /* R0..R252: the top two declared GPRs are the hardware's */
#define NV_NUM_SCRATCH 4     /* R0..R3: per-instruction temporaries */
#define NV_MAX_SCOPE   16    /* B0..B15 */
/* Every .E global access names a uniform register that must hold
 * c[0x0][0x118] (zero at launch), exactly as nvcc emits it: GA10B faults with
 * INVALID_ADDR_SPACE when a buffer address is placed there instead. */
#define NV_GLOBAL_UR       4
#define NV_CB0_GLOBAL_BASE 0x118

typedef struct {
    uint8_t is_const;
    uint8_t neg, abs;   /* float source modifiers of a folded fneg / fabs */
    uint32_t imm;
    int reg;
} opnd;

/* nv_ctx.fold[]: this fneg / fabs has no register, its users apply it as a
 * source modifier on its operand */
#define FOLD_NEG 1
#define FOLD_ABS 2

typedef struct {
    uint32_t start, end;
    uint32_t def;
    uint8_t ncomp;
} interval;

typedef struct {
    int merge;       /* label of this scope's BSYNC */
    int escapes;     /* break/continue paths that leave through this merge */
    int targeted;    /* an escaping path lands here: reset the escape register */
} nv_scope;

typedef struct {
    nir_function_impl* impl;
    nv_asm* out;
    char log[256];
    int failed;

    /* emission-order numbering */
    uint32_t pos;
    uint32_t* block_end;
    uint32_t* loop_start;
    uint32_t* loop_end;
    unsigned nloops, loop_iter;
    unsigned loop_stack[NV_MAX_SCOPE];
    int loop_sp;

    /* registers */
    interval* iv;
    int* def_reg;
    uint8_t* fold;
    unsigned ndefs;
    uint8_t used[256];
    int first_alloc;
    int max_reg;     /* highest GPR index used + 1 */
    int esc_reg;     /* escape depth register, -1 when there are no jumps */
    int ntmp;

    /* control flow */
    nv_scope scope[NV_MAX_SCOPE];
    int nscope;
    int loop_scope[NV_MAX_SCOPE];
    int nloop_scope;
    uint8_t ssbo_used[NV_MAX_SSBO];
} nv_ctx;

static void fail(nv_ctx* c, const char* fmt, ...) {
    if (c->failed)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->log, sizeof(c->log), fmt, ap);
    va_end(ap);
    c->failed = 1;
}

/* ---- scheduling control ------------------------------------------------- */
static nv_ctrl ct_make(int has_dst, int has_src, int delay) {
    nv_ctrl k = nv_ctrl_none();
    k.wt_mask = 0x3;
    if (has_dst) k.wr_bar = 0;
    if (has_src) k.rd_bar = 1;
    k.delay = (uint8_t)delay;
    return k;
}
static nv_ctrl ct_alu(void)   { return ct_make(1, 1, 6); }
static nv_ctrl ct_pred(void)  { return ct_make(1, 1, 13); }
static nv_ctrl ct_nosrc(void) { return ct_make(1, 0, 6); }
static nv_ctrl ct_store(void) { return ct_make(0, 1, 4); }
static nv_ctrl ct_branch(void) {
    nv_ctrl k = nv_ctrl_none();
    k.wt_mask = 0x3f; k.delay = 5; k.yld = 1;
    return k;
}
static nv_ctrl ct_sync(int delay) {
    nv_ctrl k = nv_ctrl_none();
    k.delay = (uint8_t)delay; k.yld = 1;
    return k;
}

/* ---- operands ----------------------------------------------------------- */
static void emit(nv_ctx* c, const uint32_t w[4]) {
    nv_asm_emit(c->out, w);
}

static void e_mov(nv_ctx* c, int dst, nv_src s) {
    uint32_t w[4];
    nv_mov(w, (uint8_t)dst, s, ct_alu());
    emit(c, w);
}

static bool def_const(nir_def* d, unsigned comp, uint32_t* v) {
    nir_instr* in = nir_def_instr(d);
    if (in->type == nir_instr_type_load_const) {
        *v = nir_instr_as_load_const(in)->value[comp].u32;
        return true;
    }
    if (in->type == nir_instr_type_undef) {
        *v = 0;
        return true;
    }
    return false;
}

static opnd get_opnd(nv_ctx* c, nir_def* d, unsigned comp) {
    opnd o;
    memset(&o, 0, sizeof(o));
    if (def_const(d, comp, &o.imm)) {
        o.is_const = 1;
        return o;
    }
    int r = d->index < c->ndefs ? c->def_reg[d->index] : -1;
    if (r < 0) {
        fail(c, "use of unallocated def %u", d->index);
        r = 0;
    }
    o.reg = r + (int)comp;
    return o;
}

static opnd alu_opnd(nv_ctx* c, nir_alu_instr* alu, unsigned i) {
    nir_def* d = alu->src[i].src.ssa;
    uint8_t f = d->index < c->ndefs ? c->fold[d->index] : 0;
    if (!f)
        return get_opnd(c, d, alu->src[i].swizzle[0]);
    nir_alu_instr* m = nir_instr_as_alu(nir_def_instr(d));
    opnd o = get_opnd(c, m->src[0].src.ssa, m->src[0].swizzle[0]);
    o.neg = f == FOLD_NEG;
    o.abs = f == FOLD_ABS;
    return o;
}

static int dst_reg(nv_ctx* c, nir_def* d) {
    int r = d->index < c->ndefs ? c->def_reg[d->index] : -1;
    if (r < 0) {
        fail(c, "def %u has no register", d->index);
        r = 0;
    }
    return r;
}

static int tmp_reg(nv_ctx* c) {
    if (c->ntmp >= NV_NUM_SCRATCH) {
        fail(c, "out of scratch registers");
        return 0;
    }
    return c->ntmp++;
}

/* any encodable source: register, RZ or immediate */
static nv_src s_any(opnd o) {
    if (o.is_const)
        return o.imm ? nv_imm(o.imm) : nv_zero();
    nv_src s = nv_gpr((uint8_t)o.reg);
    s.neg = o.neg;
    s.abs = o.abs;
    return s;
}

/* negated float source */
static nv_src s_fneg(nv_src s) {
    if (s.kind == NV_SRC_IMM)
        s.imm ^= 0x80000000u;
    else
        s.neg ^= 1;
    return s;
}

/* register-only slot: a non-zero constant is materialized in a scratch GPR */
static nv_src s_reg(nv_ctx* c, opnd o) {
    if (!o.is_const)
        return s_any(o);
    if (o.imm == 0)
        return nv_zero();
    int t = tmp_reg(c);
    e_mov(c, t, nv_imm(o.imm));
    return nv_gpr((uint8_t)t);
}

static void commute(opnd* a, opnd* b) {
    if (a->is_const && !b->is_const) {
        opnd t = *a; *a = *b; *b = t;
    }
}

static nv_src neg_rz(void) {
    nv_src z = nv_zero();
    z.neg = 1;
    return z;
}

/* ---- floor without the rounding unit ------------------------------------ *
 * FRND goes through the same unit as MUFU and costs about as much as 16
 * FADDs on GA10B.  For |x| < 2^22, x + 1.5*2^23 lies in [2^23, 2^24), where
 * the floats are exactly the integers, so that sum rounded down is
 * floor(x) + bias and taking the bias off again is exact. */
#define FLOOR_BIAS  0x4b400000u   /* 1.5 * 2^23 */
#define FLOOR_RANGE 0x4a800000u   /* 2^22 */

/* an upper bound of |src i|, infinity when nothing is known */
static float src_bound(nir_alu_instr* alu, unsigned i, int depth) {
    nir_def* d = alu->src[i].src.ssa;
    nir_instr* in = nir_def_instr(d);
    if (d->bit_size != 32)
        return INFINITY;
    if (in->type == nir_instr_type_load_const) {
        float v = fabsf(nir_instr_as_load_const(in)->value[alu->src[i].swizzle[0]].f32);
        return v == v ? v : INFINITY;
    }
    if (in->type != nir_instr_type_alu || depth == 0)
        return INFINITY;
    nir_alu_instr* a = nir_instr_as_alu(in);
    float x, y;
    switch (a->op) {
    case nir_op_fsin: case nir_op_fcos:
    case nir_op_fsin_normalized_2_pi: case nir_op_fcos_normalized_2_pi:
    case nir_op_fsat: case nir_op_ffract:
        return 1.0f;
    case nir_op_fneg: case nir_op_fabs:
        return src_bound(a, 0, depth - 1);
    case nir_op_fmul:
        x = src_bound(a, 0, depth - 1);
        y = src_bound(a, 1, depth - 1);
        return isfinite(x) && isfinite(y) ? x * y : INFINITY;
    case nir_op_fadd: case nir_op_fsub:
        return src_bound(a, 0, depth - 1) + src_bound(a, 1, depth - 1);
    default:
        return INFINITY;
    }
}

/* dst = floor(x); dst must not be x's register */
static void e_floor(nv_ctx* c, int dst, nv_src x, int in_range) {
    uint32_t w[4];
    nv_src t = nv_gpr((uint8_t)dst);
    nv_fadd(w, (uint8_t)dst, x, nv_imm(FLOOR_BIAS), 0, 0, NV_RND_NEGINF, ct_alu());
    emit(c, w);
    nv_fadd(w, (uint8_t)dst, t, nv_imm(FLOOR_BIAS ^ 0x80000000u), 0, 0,
            NV_RND_NEAREST, ct_alu());
    emit(c, w);
    if (in_range)
        return;
    /* anything else, NaN included, takes the real thing */
    nv_src ax = x;
    ax.abs = 1;
    nv_fsetp(w, 0, ax, nv_imm(FLOOR_RANGE), NV_FCMP_OLT, NV_SET_AND, nv_pt(), 0,
             ct_pred());
    emit(c, w);
    nv_frnd(w, (uint8_t)dst, x, 32, NV_RND_NEGINF, 0, ct_alu());
    nv_set_guard(w, nv_pn(0));
    emit(c, w);
}

/* ---- ALU emission ------------------------------------------------------- */
static int flip_icmp(int op) {
    switch (op) {
    case NV_ICMP_LT: return NV_ICMP_GT;
    case NV_ICMP_GT: return NV_ICMP_LT;
    case NV_ICMP_LE: return NV_ICMP_GE;
    case NV_ICMP_GE: return NV_ICMP_LE;
    default: return op;
    }
}

static int flip_fcmp(int op) {
    switch (op) {
    case NV_FCMP_OLT: return NV_FCMP_OGT;
    case NV_FCMP_OGT: return NV_FCMP_OLT;
    case NV_FCMP_OLE: return NV_FCMP_OGE;
    case NV_FCMP_OGE: return NV_FCMP_OLE;
    case NV_FCMP_LT: return NV_FCMP_GT;
    case NV_FCMP_GT: return NV_FCMP_LT;
    case NV_FCMP_LE: return NV_FCMP_GE;
    case NV_FCMP_GE: return NV_FCMP_LE;
    default: return op;
    }
}

/* P0 = (x != 0) for an int32-lowered bool */
static void pred_from_bool(nv_ctx* c, opnd x) {
    uint32_t w[4];
    nv_isetp(w, 0, s_reg(c, x), nv_zero(), NV_ICMP_NE, NV_SET_AND, nv_pt(),
             0, 0, ct_pred());
    emit(c, w);
}

/* dst = P0 ? ~0 : 0 (SEL src0 is register-only, hence the inverted select) */
static void bool_from_pred(nv_ctx* c, int dr) {
    uint32_t w[4];
    nv_sel(w, (uint8_t)dr, nv_zero(), nv_imm(0xffffffffu), nv_pn(0), ct_alu());
    emit(c, w);
}

static void emit_icmp(nv_ctx* c, int dr, opnd a, opnd b, int cmp, int is_signed) {
    uint32_t w[4];
    if (a.is_const && !b.is_const) {
        opnd t = a; a = b; b = t;
        cmp = flip_icmp(cmp);
    }
    nv_isetp(w, 0, s_reg(c, a), s_any(b), (enum nvicmp)cmp, NV_SET_AND,
             nv_pt(), is_signed, 0, ct_pred());
    emit(c, w);
    bool_from_pred(c, dr);
}

static void emit_fcmp(nv_ctx* c, int dr, opnd a, opnd b, int cmp) {
    uint32_t w[4];
    if (a.is_const && !b.is_const) {
        opnd t = a; a = b; b = t;
        cmp = flip_fcmp(cmp);
    }
    nv_fsetp(w, 0, s_reg(c, a), s_any(b), (enum nvfcmp)cmp, NV_SET_AND,
             nv_pt(), 0, ct_pred());
    emit(c, w);
    bool_from_pred(c, dr);
}

static void emit_mufu(nv_ctx* c, int dr, nv_src s, uint8_t op) {
    uint32_t w[4];
    nv_mufu(w, (uint8_t)dr, s, op, ct_alu());
    emit(c, w);
}

static void emit_alu(nv_ctx* c, nir_alu_instr* alu) {
    nir_def* def = &alu->def;
    unsigned nin = nir_op_infos[alu->op].num_inputs;
    if (def->index < c->ndefs && c->fold[def->index])
        return;
    int dr = dst_reg(c, def);
    uint8_t d8 = (uint8_t)dr;
    uint32_t w[4];
    nv_ctrl ct = ct_alu();

    if (alu->op == nir_op_mov) {
        for (unsigned i = 0; i < def->num_components; i++)
            e_mov(c, dr + (int)i,
                  s_any(get_opnd(c, alu->src[0].src.ssa, alu->src[0].swizzle[i])));
        return;
    }
    if (alu->op == nir_op_vec2 || alu->op == nir_op_vec3 ||
        alu->op == nir_op_vec4) {
        for (unsigned i = 0; i < nin; i++)
            e_mov(c, dr + (int)i, s_any(alu_opnd(c, alu, i)));
        return;
    }
    if (def->num_components != 1) {
        fail(c, "vector ALU op %s survived scalarization",
             nir_op_infos[alu->op].name);
        return;
    }

    opnd A, B, C;
    memset(&A, 0, sizeof(A)); B = A; C = A;
    if (nin >= 1) A = alu_opnd(c, alu, 0);
    if (nin >= 2) B = alu_opnd(c, alu, 1);
    if (nin >= 3) C = alu_opnd(c, alu, 2);

    switch (alu->op) {
    /* ---- float ---- */
    case nir_op_fadd:
        commute(&A, &B);
        nv_fadd(w, d8, s_reg(c, A), s_any(B), 0, 0, NV_RND_NEAREST, ct);
        break;
    case nir_op_fsub:
        /* const - x is -x + const: the immediate only fits the second slot */
        if (A.is_const && !B.is_const)
            nv_fadd(w, d8, s_fneg(s_any(B)), s_any(A), 0, 0, NV_RND_NEAREST, ct);
        else if (B.is_const)
            nv_fadd(w, d8, s_reg(c, A), nv_imm(B.imm ^ 0x80000000u), 0, 0,
                    NV_RND_NEAREST, ct);
        else
            nv_fadd(w, d8, s_reg(c, A), s_fneg(s_any(B)), 0, 0, NV_RND_NEAREST, ct);
        break;
    case nir_op_fmul:
        commute(&A, &B);
        /* mark_folds admits one |x| operand; nvcc only puts it first */
        if (B.abs) {
            opnd t = A; A = B; B = t;
        }
        nv_fmul(w, d8, s_reg(c, A), s_any(B), 0, 0, NV_RND_NEAREST, ct);
        break;
    case nir_op_ffma: {
        /* a negated product is encoded on the second factor, as nvcc does */
        int pneg = A.neg ^ B.neg;
        A.neg = B.neg = 0;
        commute(&A, &B);
        nv_src a = s_reg(c, A), b = s_any(B), cc = s_any(C);
        if (pneg)
            b = s_fneg(b);
        if (b.kind == NV_SRC_IMM && cc.kind == NV_SRC_IMM) {
            int t = tmp_reg(c);
            e_mov(c, t, b);
            b = nv_gpr((uint8_t)t);
        }
        nv_ffma(w, d8, a, b, cc, 0, 0, NV_RND_NEAREST, ct);
        break;
    }
    case nir_op_fmin:
        commute(&A, &B);
        nv_fmnmx(w, d8, s_reg(c, A), s_any(B), nv_pt(), 0, ct);
        break;
    case nir_op_fmax:
        commute(&A, &B);
        nv_fmnmx(w, d8, s_reg(c, A), s_any(B), nv_pn(NV_PT), 0, ct);
        break;
    case nir_op_fneg: {
        nv_src a = s_reg(c, A);
        a.neg = 1;
        nv_fadd(w, d8, a, neg_rz(), 0, 0, NV_RND_NEAREST, ct);
        break;
    }
    case nir_op_fabs: {
        nv_src a = s_reg(c, A);
        a.abs = 1;
        nv_fadd(w, d8, a, neg_rz(), 0, 0, NV_RND_NEAREST, ct);
        break;
    }
    case nir_op_fsat:
        nv_fadd(w, d8, s_reg(c, A), neg_rz(), 1, 0, NV_RND_NEAREST, ct);
        break;
    case nir_op_fsin:
    case nir_op_fcos:
        /* MUFU.SIN/COS take the angle in turns: scale by 1/(2*pi) first */
        nv_fmul(w, d8, s_reg(c, A), nv_imm(0x3e22f983u), 0, 0,
                NV_RND_NEAREST, ct);
        emit(c, w);
        emit_mufu(c, dr, nv_gpr(d8),
                  alu->op == nir_op_fsin ? NV_MUFU_SIN : NV_MUFU_COS);
        return;
    case nir_op_fsin_normalized_2_pi:
        emit_mufu(c, dr, s_any(A), NV_MUFU_SIN); return;
    case nir_op_fcos_normalized_2_pi:
        emit_mufu(c, dr, s_any(A), NV_MUFU_COS); return;
    case nir_op_fexp2: emit_mufu(c, dr, s_any(A), NV_MUFU_EX2); return;
    case nir_op_flog2: emit_mufu(c, dr, s_any(A), NV_MUFU_LG2); return;
    case nir_op_frcp:  emit_mufu(c, dr, s_any(A), NV_MUFU_RCP); return;
    case nir_op_frsq:  emit_mufu(c, dr, s_any(A), NV_MUFU_RSQ); return;
    case nir_op_fsqrt: emit_mufu(c, dr, s_any(A), NV_MUFU_SQRT); return;
    case nir_op_fdiv:
        emit_mufu(c, dr, s_any(B), NV_MUFU_RCP);
        nv_fmul(w, d8, nv_gpr(d8), s_any(A), 0, 0, NV_RND_NEAREST, ct);
        break;
    case nir_op_fpow:
        emit_mufu(c, dr, s_any(A), NV_MUFU_LG2);
        nv_fmul(w, d8, nv_gpr(d8), s_any(B), 0, 0, NV_RND_NEAREST, ct);
        emit(c, w);
        emit_mufu(c, dr, nv_gpr(d8), NV_MUFU_EX2);
        return;
    case nir_op_ffloor: {
        /* the biased sum turns floor(-0.0) into +0.0 */
        if (alu->fp_math_ctrl & nir_fp_preserve_signed_zero) {
            nv_frnd(w, d8, s_any(A), 32, NV_RND_NEGINF, 0, ct);
            break;
        }
        nv_src a = s_reg(c, A);
        int in_range = src_bound(alu, 0, 4) < 2097152.0f;
        if (a.reg != d8) {
            e_floor(c, dr, a, in_range);
            return;
        }
        int t = tmp_reg(c);
        e_floor(c, t, a, in_range);
        e_mov(c, dr, nv_gpr((uint8_t)t));
        return;
    }
    case nir_op_fceil:
        nv_frnd(w, d8, s_any(A), 32, NV_RND_POSINF, 0, ct); break;
    case nir_op_ftrunc:
        nv_frnd(w, d8, s_any(A), 32, NV_RND_ZERO, 0, ct); break;
    case nir_op_fround_even:
        nv_frnd(w, d8, s_any(A), 32, NV_RND_NEAREST, 0, ct); break;
    case nir_op_ffract: {
        nv_src a = s_reg(c, A), f = nv_gpr((uint8_t)tmp_reg(c));
        if (alu->fp_math_ctrl & nir_fp_preserve_signed_zero) {
            nv_frnd(w, f.reg, a, 32, NV_RND_NEGINF, 0, ct);
            emit(c, w);
        } else {
            e_floor(c, f.reg, a, src_bound(alu, 0, 4) < 2097152.0f);
        }
        f.neg = 1;
        nv_fadd(w, d8, a, f, 0, 0, NV_RND_NEAREST, ct);
        break;
    }

    /* ---- conversions ---- */
    case nir_op_f2i32:
        nv_f2i(w, d8, s_any(A), 1, 32, 32, NV_RND_ZERO, 0, ct); break;
    case nir_op_f2u32:
        nv_f2i(w, d8, s_any(A), 0, 32, 32, NV_RND_ZERO, 0, ct); break;
    case nir_op_i2f32:
        nv_i2f(w, d8, s_any(A), 1, 32, 32, NV_RND_NEAREST, ct); break;
    case nir_op_u2f32:
        nv_i2f(w, d8, s_any(A), 0, 32, 32, NV_RND_NEAREST, ct); break;
    /* int32 bools are canonical 0 / ~0, so b2f/b2i are a mask */
    case nir_op_b2f32:
        nv_lop3(w, d8, s_reg(c, A), nv_imm(0x3f800000u), nv_zero(), 0xc0,
                nv_pt(), ct);
        break;
    case nir_op_b2i32:
        nv_lop3(w, d8, s_reg(c, A), nv_imm(1), nv_zero(), 0xc0, nv_pt(), ct);
        break;
    case nir_op_b2b32:
        e_mov(c, dr, s_any(A)); return;

    /* ---- integer ---- */
    case nir_op_iadd:
        commute(&A, &B);
        nv_iadd3(w, d8, s_reg(c, A), s_any(B), nv_zero(), ct);
        break;
    case nir_op_isub: {
        nv_src a = s_reg(c, A), b;
        if (B.is_const) {
            b = s_any((opnd){ .is_const = 1, .imm = 0u - B.imm });
        } else {
            b = nv_gpr((uint8_t)B.reg);
            b.neg = 1;
        }
        nv_iadd3(w, d8, a, b, nv_zero(), ct);
        break;
    }
    case nir_op_ineg: {
        nv_src a = s_reg(c, A);
        a.neg = 1;
        nv_iadd3(w, d8, nv_zero(), a, nv_zero(), ct);
        break;
    }
    case nir_op_imul:
        commute(&A, &B);
        nv_imad(w, d8, s_reg(c, A), s_any(B), nv_zero(), 0, ct);
        break;
    case nir_op_imin: case nir_op_umin:
        commute(&A, &B);
        nv_imnmx(w, d8, s_reg(c, A), s_any(B), nv_pt(),
                 alu->op == nir_op_imin, ct);
        break;
    case nir_op_imax: case nir_op_umax:
        commute(&A, &B);
        nv_imnmx(w, d8, s_reg(c, A), s_any(B), nv_pn(NV_PT),
                 alu->op == nir_op_imax, ct);
        break;
    case nir_op_iand:
        commute(&A, &B);
        nv_lop3(w, d8, s_reg(c, A), s_any(B), nv_zero(), 0xc0, nv_pt(), ct);
        break;
    case nir_op_ior:
        commute(&A, &B);
        nv_lop3(w, d8, s_reg(c, A), s_any(B), nv_zero(), 0xfc, nv_pt(), ct);
        break;
    case nir_op_ixor:
        commute(&A, &B);
        nv_lop3(w, d8, s_reg(c, A), s_any(B), nv_zero(), 0x3c, nv_pt(), ct);
        break;
    case nir_op_inot:
        nv_lop3(w, d8, s_reg(c, A), nv_zero(), nv_zero(), 0x0f, nv_pt(), ct);
        break;
    case nir_op_ishl:
        nv_shf(w, d8, s_reg(c, A), nv_zero(), s_any(B), 0, 1, 0, 0, 0, ct);
        break;
    case nir_op_ishr:
        nv_shf(w, d8, nv_zero(), s_reg(c, A), s_any(B), 1, 1, 0, 1, 1, ct);
        break;
    case nir_op_ushr:
        nv_shf(w, d8, nv_zero(), s_reg(c, A), s_any(B), 1, 1, 0, 0, 1, ct);
        break;

    /* ---- select / compare ---- */
    case nir_op_bcsel: case nir_op_b32csel:
        pred_from_bool(c, A);
        if (B.is_const && !C.is_const)
            nv_sel(w, d8, s_reg(c, C), s_any(B), nv_pn(0), ct);
        else
            nv_sel(w, d8, s_reg(c, B), s_any(C), nv_p(0), ct);
        break;
    case nir_op_ilt: case nir_op_ilt32:
        emit_icmp(c, dr, A, B, NV_ICMP_LT, 1); return;
    case nir_op_ige: case nir_op_ige32:
        emit_icmp(c, dr, A, B, NV_ICMP_GE, 1); return;
    case nir_op_ieq: case nir_op_ieq32:
        emit_icmp(c, dr, A, B, NV_ICMP_EQ, 1); return;
    case nir_op_ine: case nir_op_ine32:
        emit_icmp(c, dr, A, B, NV_ICMP_NE, 1); return;
    case nir_op_ult: case nir_op_ult32:
        emit_icmp(c, dr, A, B, NV_ICMP_LT, 0); return;
    case nir_op_uge: case nir_op_uge32:
        emit_icmp(c, dr, A, B, NV_ICMP_GE, 0); return;
    case nir_op_flt: case nir_op_flt32:
        emit_fcmp(c, dr, A, B, NV_FCMP_OLT); return;
    case nir_op_fge: case nir_op_fge32:
        emit_fcmp(c, dr, A, B, NV_FCMP_OGE); return;
    case nir_op_feq: case nir_op_feq32:
        emit_fcmp(c, dr, A, B, NV_FCMP_OEQ); return;
    case nir_op_fneu: case nir_op_fneu32:
        emit_fcmp(c, dr, A, B, NV_FCMP_NE); return;
    case nir_op_fneo: case nir_op_fneo32:
        emit_fcmp(c, dr, A, B, NV_FCMP_ONE); return;
    case nir_op_fequ: case nir_op_fequ32:
        emit_fcmp(c, dr, A, B, NV_FCMP_EQ); return;
    case nir_op_fltu: case nir_op_fltu32:
        emit_fcmp(c, dr, A, B, NV_FCMP_LT); return;
    case nir_op_fgeu: case nir_op_fgeu32:
        emit_fcmp(c, dr, A, B, NV_FCMP_GE); return;

    default:
        fail(c, "unsupported ALU op %s", nir_op_infos[alu->op].name);
        return;
    }
    emit(c, w);
}

/* ---- intrinsic emission ------------------------------------------------- */
/* Evaluate a compile-time-constant component of a def through load_const /
 * vecN / mov+swizzle chains. */
static bool eval_const_comp(nir_def* d, unsigned comp, uint32_t* out) {
    if (!d || comp >= d->num_components)
        return false;
    if (def_const(d, comp, out))
        return true;
    nir_instr* instr = nir_def_instr(d);
    if (instr->type != nir_instr_type_alu)
        return false;
    nir_alu_instr* alu = nir_instr_as_alu(instr);
    switch (alu->op) {
    case nir_op_mov:
        return eval_const_comp(alu->src[0].src.ssa,
                               alu->src[0].swizzle[comp], out);
    case nir_op_vec2: case nir_op_vec3: case nir_op_vec4:
        return eval_const_comp(alu->src[comp].src.ssa,
                               alu->src[comp].swizzle[0], out);
    default:
        return false;
    }
}

static int ssbo_block(nv_ctx* c, nir_src src) {
    uint32_t block;
    if (!eval_const_comp(src.ssa, 0, &block)) {
        fail(c, "non-constant SSBO block index");
        return 0;
    }
    if (block >= NV_MAX_SSBO) {
        fail(c, "SSBO block %u exceeds %u", block, NV_MAX_SSBO);
        return 0;
    }
    return (int)block;
}

/* 64-bit address base + offset in the even-aligned scratch pair R0:R1 */
static int ssbo_addr_pair(nv_ctx* c, int block, opnd off) {
    uint32_t w[4];
    c->ntmp = 2;
    e_mov(c, 0, s_any(off));
    e_mov(c, 1, nv_imm(1));
    nv_imad_wide(w, 0, nv_gpr(0), nv_gpr(1),
                 nv_cb(0, NV_SSBO_BASE_OFF + (uint32_t)block * 8), 0, ct_alu());
    emit(c, w);
    return 0;
}

static void emit_s2r3(nv_ctx* c, int dr, uint8_t sr) {
    uint32_t w[4];
    for (int i = 0; i < 3; i++) {
        nv_s2r(w, (uint8_t)(dr + i), (uint8_t)(sr + i), ct_nosrc());
        emit(c, w);
    }
}

static void emit_intrinsic(nv_ctx* c, nir_intrinsic_instr* intr) {
    uint32_t w[4];
    bool has_def = nir_intrinsic_infos[intr->intrinsic].has_dest;
    int dr = has_def ? dst_reg(c, &intr->def) : 0;
    unsigned ncomp = has_def ? intr->def.num_components : 0;

    switch (intr->intrinsic) {
    case nir_intrinsic_load_local_invocation_id:
        emit_s2r3(c, dr, NV_SR_TID_X);
        return;
    case nir_intrinsic_load_workgroup_id:
        emit_s2r3(c, dr, NV_SR_CTAID_X);
        return;
    case nir_intrinsic_load_base_workgroup_id:
    case nir_intrinsic_load_base_global_invocation_id:
        /* a plain (non-offset) dispatch */
        for (unsigned i = 0; i < ncomp; i++)
            e_mov(c, dr + (int)i, nv_zero());
        return;
    case nir_intrinsic_load_num_workgroups:
        for (unsigned i = 0; i < ncomp; i++)
            e_mov(c, dr + (int)i, nv_cb(0, 0x0c + 4 * i));
        return;
    case nir_intrinsic_load_uniform: {
        uint32_t ioff;
        if (!eval_const_comp(intr->src[0].ssa, 0, &ioff)) {
            fail(c, "indirect uniform load");
            return;
        }
        unsigned off = (unsigned)nir_intrinsic_base(intr) + ioff;
        /* the uniform window, or the texture descriptor table nv_tex.c reads
         * (base NV_TEXDESC_OFF - NV_UNIFORM_BASE_OFF) */
        bool in_unif = off + 4 * ncomp <= NV_SSBO_BASE_OFF - NV_UNIFORM_BASE_OFF;
        bool in_desc = off >= NV_TEXDESC_OFF - NV_UNIFORM_BASE_OFF &&
                       off + 4 * ncomp <= NV_CB0_SIZE - NV_UNIFORM_BASE_OFF;
        if (!in_unif && !in_desc) {
            fail(c, "uniform offset %u outside the cb0 uniform window", off);
            return;
        }
        for (unsigned i = 0; i < ncomp; i++)
            e_mov(c, dr + (int)i, nv_cb(0, NV_UNIFORM_BASE_OFF + off + 4 * i));
        return;
    }
    case nir_intrinsic_load_ssbo: {
        int ur = NV_GLOBAL_UR;
        int ap = ssbo_addr_pair(c, ssbo_block(c, intr->src[0]),
                                get_opnd(c, intr->src[1].ssa, 0));
        for (unsigned i = 0; i < ncomp; i++) {
            nv_ldg(w, (uint8_t)(dr + (int)i), nv_gpr((uint8_t)ap), 1,
                   nv_ugpr64((uint8_t)ur), (int32_t)(4 * i), NV_MT_B32,
                   NV_ORD_WEAK, NV_EV_NORMAL, 0, ct_alu());
            emit(c, w);
        }
        return;
    }
    case nir_intrinsic_store_ssbo: {
        nir_def* val = intr->src[0].ssa;
        unsigned mask = nir_intrinsic_write_mask(intr);
        int ur = NV_GLOBAL_UR;
        int ap = ssbo_addr_pair(c, ssbo_block(c, intr->src[1]),
                                get_opnd(c, intr->src[2].ssa, 0));
        for (unsigned i = 0; i < val->num_components; i++) {
            if (!(mask & (1u << i)))
                continue;
            opnd v = get_opnd(c, val, i);
            nv_src data = s_any(v);
            if (data.kind == NV_SRC_IMM) {
                e_mov(c, 2, data);
                data = nv_gpr(2);
            }
            nv_stg(w, nv_gpr((uint8_t)ap), 1, nv_ugpr64((uint8_t)ur), data,
                   (int32_t)(4 * i), NV_MT_B32, NV_ORD_WEAK, NV_EV_NORMAL,
                   ct_store());
            emit(c, w);
        }
        return;
    }
    case nir_intrinsic_ssbo_atomic: {
        uint32_t subop;
        switch (nir_intrinsic_atomic_op(intr)) {
        case nir_atomic_op_iadd: subop = 0; break;
        case nir_atomic_op_umin: subop = 1; break;
        case nir_atomic_op_umax: subop = 2; break;
        default:
            fail(c, "unsupported ssbo atomic op %d",
                 (int)nir_intrinsic_atomic_op(intr));
            return;
        }
        int block = ssbo_block(c, intr->src[0]);
        int ap = ssbo_addr_pair(c, block, get_opnd(c, intr->src[1].ssa, 0));
        opnd v = get_opnd(c, intr->src[2].ssa, 0);
        nv_src data = s_any(v);
        if (data.kind == NV_SRC_IMM) {
            e_mov(c, 2, data);
            data = nv_gpr(2);
        }
        nv_atomg(w, (uint8_t)dr, nv_gpr((uint8_t)ap), 1, data, 0, subop,
                 ct_alu());
        emit(c, w);
        return;
    }
    case nir_intrinsic_load_shared: {
        nv_src addr = s_reg(c, get_opnd(c, intr->src[0].ssa, 0));
        int32_t base = (int32_t)nir_intrinsic_base(intr);
        for (unsigned i = 0; i < ncomp; i++) {
            nv_lds(w, (uint8_t)(dr + (int)i), addr, base + (int32_t)(4 * i),
                   NV_MT_B32, ct_alu());
            emit(c, w);
        }
        return;
    }
    case nir_intrinsic_store_shared: {
        nir_def* val = intr->src[0].ssa;
        unsigned mask = nir_intrinsic_write_mask(intr);
        nv_src addr = s_reg(c, get_opnd(c, intr->src[1].ssa, 0));
        int32_t base = (int32_t)nir_intrinsic_base(intr);
        for (unsigned i = 0; i < val->num_components; i++) {
            if (!(mask & (1u << i)))
                continue;
            int save = c->ntmp;
            nv_sts(w, addr, s_reg(c, get_opnd(c, val, i)),
                   base + (int32_t)(4 * i), NV_MT_B32, ct_store());
            emit(c, w);
            c->ntmp = save;
        }
        return;
    }
    default:
        fail(c, "unsupported intrinsic %s",
             nir_intrinsic_infos[intr->intrinsic].name);
        return;
    }
}

/* ---- control flow ------------------------------------------------------- */
static void emit_cf_list(nv_ctx* c, struct exec_list* list);

static void e_bra(nv_ctx* c, int label) {
    nv_asm_bra(c->out, label, nv_pt(), ct_branch());
}

/* @P0 / @!P0 BRA: the condition is the guard, as nvcc emits it */
static void e_bra_if(nv_ctx* c, int label, nv_pred guard) {
    nv_asm_bra(c->out, label, nv_pt(), ct_branch());
    if (!c->out->overflow)
        nv_set_guard(c->out->w[c->out->n - 1], guard);
}

static int push_scope(nv_ctx* c) {
    if (c->nscope >= NV_MAX_SCOPE) {
        fail(c, "control flow nested deeper than %d", NV_MAX_SCOPE);
        return 0;
    }
    int d = c->nscope++;
    c->scope[d].merge = nv_asm_label(c->out);
    c->scope[d].escapes = 0;
    c->scope[d].targeted = 0;
    /* barrier registers hold garbage at kernel entry: clear before arming */
    uint32_t w[4];
    nv_bclear(w, (uint8_t)d, ct_sync(1));
    emit(c, w);
    nv_asm_bssy(c->out, (uint8_t)d, c->scope[d].merge, nv_pt(), ct_sync(1));
    return d;
}

static void pop_scope(nv_ctx* c) {
    int d = --c->nscope;
    nv_scope s = c->scope[d];
    uint32_t w[4];
    nv_asm_bind(c->out, s.merge);
    nv_bsync(w, (uint8_t)d, nv_pt(), ct_sync(5));
    emit(c, w);
    if (s.escapes) {
        /* lanes heading for an outer scope carry its depth: pass them on */
        nv_isetp(w, 0, nv_gpr((uint8_t)c->esc_reg), nv_imm((uint32_t)d),
                 NV_ICMP_LT, NV_SET_AND, nv_pt(), 0, 0, ct_pred());
        emit(c, w);
        e_bra_if(c, c->scope[d - 1].merge, nv_p(0));
    }
    if (s.targeted)
        e_mov(c, c->esc_reg, nv_imm(0xffffffffu));
}

/* leave for scope `target` through every enclosing merge point */
static void emit_escape(nv_ctx* c, int target) {
    int inner = c->nscope - 1;
    if (target < 0 || target > inner) {
        fail(c, "jump to a scope that is not open");
        return;
    }
    if (target != inner) {
        e_mov(c, c->esc_reg, nv_imm((uint32_t)target));
        for (int i = inner; i > target; i--)
            c->scope[i].escapes++;
        c->scope[target].targeted = 1;
    }
    e_bra(c, c->scope[inner].merge);
}

typedef struct {
    int dst;
    opnd src;
    int done;
} pmove;

/* Phi copies leaving `block`, as one parallel copy sequentialized here. */
static void emit_phi_moves(nv_ctx* c, nir_block* block) {
    pmove* mv = NULL;
    unsigned n = 0, cap = 0;
    for (int s = 0; s < 2; s++) {
        nir_block* succ = block->successors[s];
        if (!succ)
            continue;
        nir_foreach_phi(phi, succ) {
            nir_foreach_phi_src(ps, phi) {
                if (ps->pred != block)
                    continue;
                int dr = dst_reg(c, &phi->def);
                for (unsigned i = 0; i < phi->def.num_components; i++) {
                    opnd src = get_opnd(c, ps->src.ssa, i);
                    if (!src.is_const && src.reg == dr + (int)i)
                        continue;
                    if (n == cap) {
                        cap = cap ? cap * 2 : 16;
                        mv = realloc(mv, cap * sizeof(*mv));
                    }
                    mv[n].dst = dr + (int)i;
                    mv[n].src = src;
                    mv[n].done = 0;
                    n++;
                }
            }
        }
    }
    c->ntmp = 0;
    unsigned left = n;
    while (left && !c->failed) {
        bool progress = false;
        for (unsigned i = 0; i < n; i++) {
            if (mv[i].done)
                continue;
            bool blocked = false;
            for (unsigned j = 0; j < n && !blocked; j++)
                blocked = j != i && !mv[j].done && !mv[j].src.is_const &&
                          mv[j].src.reg == mv[i].dst;
            if (blocked)
                continue;
            e_mov(c, mv[i].dst, s_any(mv[i].src));
            mv[i].done = 1;
            left--;
            progress = true;
        }
        if (progress)
            continue;
        /* a cycle: park one destination's old value in a scratch register */
        for (unsigned i = 0; i < n; i++) {
            if (mv[i].done)
                continue;
            int t = tmp_reg(c);
            e_mov(c, t, nv_gpr((uint8_t)mv[i].dst));
            for (unsigned j = 0; j < n; j++)
                if (!mv[j].done && !mv[j].src.is_const &&
                    mv[j].src.reg == mv[i].dst)
                    mv[j].src.reg = t;
            break;
        }
    }
    free(mv);
}

static void emit_block(nv_ctx* c, nir_block* block) {
    nir_jump_instr* jump = NULL;
    nir_foreach_instr(instr, block) {
        if (c->failed)
            return;
        c->ntmp = 0;
        switch (instr->type) {
        case nir_instr_type_alu:
            emit_alu(c, nir_instr_as_alu(instr)); break;
        case nir_instr_type_intrinsic:
            emit_intrinsic(c, nir_instr_as_intrinsic(instr)); break;
        case nir_instr_type_jump:
            jump = nir_instr_as_jump(instr); break;
        case nir_instr_type_phi:
        case nir_instr_type_load_const:
        case nir_instr_type_undef:
            break;
        default:
            fail(c, "unsupported instruction type %d", instr->type);
            return;
        }
    }
    emit_phi_moves(c, block);
    if (!jump || c->failed)
        return;
    switch (jump->type) {
    case nir_jump_return:
    case nir_jump_halt: {
        /* exited lanes never hold up a BSYNC */
        uint32_t w[4];
        nv_exit(w, ct_branch());
        emit(c, w);
        break;
    }
    case nir_jump_break:
    case nir_jump_continue:
        if (c->nloop_scope == 0) {
            fail(c, "break/continue outside a loop");
            break;
        }
        emit_escape(c, c->loop_scope[c->nloop_scope - 1] +
                       (jump->type == nir_jump_continue ? 1 : 0));
        break;
    default:
        fail(c, "unsupported jump type %d", jump->type);
    }
}

static bool list_is_trivial(struct exec_list* list) {
    if (!exec_list_is_singular(list))
        return false;
    nir_cf_node* first = exec_node_data(nir_cf_node, exec_list_get_head(list),
                                        node);
    if (first->type != nir_cf_node_block)
        return false;
    nir_block* b = nir_cf_node_as_block(first);
    if (!exec_list_is_empty(&b->instr_list))
        return false;
    nir_block* succ = b->successors[0];
    if (succ) {
        nir_instr* fi = nir_block_first_instr(succ);
        if (fi && fi->type == nir_instr_type_phi)
            return false;
    }
    return true;
}

static void emit_if(nv_ctx* c, nir_if* nif) {
    c->ntmp = 0;
    pred_from_bool(c, get_opnd(c, nif->condition.ssa, 0));
    int d = push_scope(c);
    if (c->failed)
        return;
    int merge = c->scope[d].merge;
    bool else_trivial = list_is_trivial(&nif->else_list);
    int else_label = else_trivial ? merge : nv_asm_label(c->out);
    e_bra_if(c, else_label, nv_pn(0));
    emit_cf_list(c, &nif->then_list);
    if (!else_trivial) {
        if (!nir_block_ends_in_jump(nir_if_last_then_block(nif)))
            e_bra(c, merge);
        nv_asm_bind(c->out, else_label);
        emit_cf_list(c, &nif->else_list);
    }
    if (!c->failed)
        pop_scope(c);
}

static void emit_loop(nv_ctx* c, nir_loop* loop) {
    /* break scope: armed once, lanes that left the loop wait at its BSYNC */
    int b = push_scope(c);
    int head = nv_asm_label(c->out);
    nv_asm_bind(c->out, head);
    /* continue scope: re-armed every iteration */
    push_scope(c);
    if (c->failed)
        return;
    c->loop_scope[c->nloop_scope++] = b;
    emit_cf_list(c, &loop->body);
    if (c->failed)
        return;
    pop_scope(c);
    if (nir_loop_has_continue_construct(loop))
        emit_cf_list(c, &loop->continue_list);
    c->nloop_scope--;
    e_bra(c, head);
    if (!c->failed)
        pop_scope(c);
}

static void emit_cf_list(nv_ctx* c, struct exec_list* list) {
    foreach_list_typed(nir_cf_node, node, node, list) {
        if (c->failed)
            return;
        switch (node->type) {
        case nir_cf_node_block:
            emit_block(c, nir_cf_node_as_block(node)); break;
        case nir_cf_node_if:
            emit_if(c, nir_cf_node_as_if(node)); break;
        case nir_cf_node_loop:
            emit_loop(c, nir_cf_node_as_loop(node)); break;
        default:
            fail(c, "unsupported cf node %d", node->type);
        }
    }
}

/* ---- liveness + register allocation ------------------------------------- */
/* Pass 1: number every instruction, block end (phi copies) and loop extent in
 * emission order. */
static void number_list(nv_ctx* c, struct exec_list* list) {
    foreach_list_typed(nir_cf_node, node, node, list) {
        switch (node->type) {
        case nir_cf_node_block: {
            nir_block* block = nir_cf_node_as_block(node);
            nir_foreach_instr(instr, block)
                instr->index = c->pos++;
            c->block_end[block->index] = c->pos++;
            break;
        }
        case nir_cf_node_if: {
            nir_if* nif = nir_cf_node_as_if(node);
            number_list(c, &nif->then_list);
            number_list(c, &nif->else_list);
            break;
        }
        case nir_cf_node_loop: {
            nir_loop* loop = nir_cf_node_as_loop(node);
            unsigned l = c->nloops++;
            if (c->loop_start) c->loop_start[l] = c->pos;
            number_list(c, &loop->body);
            number_list(c, &loop->continue_list);
            if (c->loop_end) c->loop_end[l] = c->pos - 1;
            break;
        }
        default:
            break;
        }
    }
}

static uint32_t def_start(nv_ctx* c, nir_def* d) {
    nir_instr* in = nir_def_instr(d);
    uint32_t s = in->index;
    if (in->type == nir_instr_type_phi) {
        nir_foreach_phi_src(ps, nir_instr_as_phi(in)) {
            uint32_t p = c->block_end[ps->pred->index];
            if (p < s) s = p;
        }
    }
    return s;
}

/* The value of `d` is needed (or written) at `pos`.  A value that enters a
 * loop from outside stays live for the whole loop. */
static void touch(nv_ctx* c, nir_def* d, uint32_t pos) {
    uint32_t dummy;
    if (def_const(d, 0, &dummy))
        return;
    if (d->bit_size != 32) {
        fail(c, "unsupported %u-bit value (def %u)", d->bit_size, d->index);
        return;
    }
    interval* iv = &c->iv[d->index];
    if (!iv->ncomp) {
        iv->ncomp = d->num_components;
        iv->def = d->index;
        iv->start = iv->end = def_start(c, d);
    }
    if (pos > iv->end)
        iv->end = pos;
    for (int i = 0; i < c->loop_sp; i++) {
        unsigned l = c->loop_stack[i];
        if (iv->start < c->loop_start[l]) {
            if (c->loop_end[l] > iv->end)
                iv->end = c->loop_end[l];
            break;
        }
    }
}

struct use_state { nv_ctx* c; uint32_t pos; };

static bool use_cb(nir_src* src, void* state) {
    struct use_state* u = (struct use_state*)state;
    nir_def* d = src->ssa;
    /* a folded modifier is read as its operand, at the user's position */
    if (u->c->fold[d->index])
        d = nir_instr_as_alu(nir_def_instr(d))->src[0].src.ssa;
    touch(u->c, d, u->pos);
    return true;
}

static bool is_fmod(nir_def* d) {
    nir_instr* in = nir_def_instr(d);
    return in->type == nir_instr_type_alu &&
           (nir_instr_as_alu(in)->op == nir_op_fneg ||
            nir_instr_as_alu(in)->op == nir_op_fabs);
}

/* An fneg / fabs whose every user can take it as a source modifier needs no
 * instruction.  The accepted forms are the ones nvcc itself emits: any
 * modifier on FADD sources, |x| on one FMUL factor, negation on FFMA. */
static void mark_folds(nv_ctx* c, nir_function_impl* impl) {
    nir_foreach_block(block, impl) {
        nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_alu)
                continue;
            nir_alu_instr* m = nir_instr_as_alu(instr);
            uint32_t k;
            if (!is_fmod(&m->def) || m->def.num_components != 1 ||
                m->def.bit_size != 32 ||
                def_const(m->src[0].src.ssa, m->src[0].swizzle[0], &k))
                continue;
            bool ok = false;
            nir_foreach_use_including_if(use, &m->def) {
                ok = false;
                if (nir_src_is_if(use) ||
                    nir_src_use_instr(use)->type != nir_instr_type_alu)
                    break;
                nir_alu_instr* u = nir_instr_as_alu(nir_src_use_instr(use));
                if (u->def.num_components != 1)
                    break;
                if (u->op == nir_op_fadd || u->op == nir_op_fsub) {
                    ok = true;
                } else if (u->op == nir_op_fmul) {
                    unsigned other = &u->src[0].src == use ? 1 : 0;
                    ok = m->op == nir_op_fabs && !is_fmod(u->src[other].src.ssa);
                } else if (u->op == nir_op_ffma) {
                    ok = m->op == nir_op_fneg;
                }
                if (!ok)
                    break;
            }
            if (ok)
                c->fold[m->def.index] = m->op == nir_op_fneg ? FOLD_NEG : FOLD_ABS;
        }
    }
}

/* Pass 2: build one live interval per value. */
static void liveness_list(nv_ctx* c, struct exec_list* list) {
    foreach_list_typed(nir_cf_node, node, node, list) {
        if (c->failed)
            return;
        switch (node->type) {
        case nir_cf_node_block: {
            nir_block* block = nir_cf_node_as_block(node);
            nir_foreach_instr(instr, block) {
                nir_def* def = nir_instr_def(instr);
                if (def && !c->fold[def->index])
                    touch(c, def, instr->index);
                if (instr->type == nir_instr_type_phi)
                    continue;
                struct use_state u = { c, instr->index };
                nir_foreach_src(instr, use_cb, &u);
            }
            uint32_t end = c->block_end[block->index];
            for (int s = 0; s < 2; s++) {
                nir_block* succ = block->successors[s];
                if (!succ)
                    continue;
                nir_foreach_phi(phi, succ) {
                    nir_foreach_phi_src(ps, phi) {
                        if (ps->pred != block)
                            continue;
                        touch(c, ps->src.ssa, end);
                        touch(c, &phi->def, end);
                    }
                }
            }
            break;
        }
        case nir_cf_node_if: {
            nir_if* nif = nir_cf_node_as_if(node);
            nir_block* prev = nir_cf_node_as_block(nir_cf_node_prev(node));
            touch(c, nif->condition.ssa, c->block_end[prev->index]);
            liveness_list(c, &nif->then_list);
            liveness_list(c, &nif->else_list);
            break;
        }
        case nir_cf_node_loop: {
            nir_loop* loop = nir_cf_node_as_loop(node);
            if (c->loop_sp >= NV_MAX_SCOPE) {
                fail(c, "loops nested deeper than %d", NV_MAX_SCOPE);
                return;
            }
            c->loop_stack[c->loop_sp++] = c->loop_iter++;
            liveness_list(c, &loop->body);
            liveness_list(c, &loop->continue_list);
            c->loop_sp--;
            break;
        }
        default:
            break;
        }
    }
}

static int iv_cmp(const void* a, const void* b) {
    const interval* x = (const interval*)a;
    const interval* y = (const interval*)b;
    if (x->start != y->start)
        return x->start < y->start ? -1 : 1;
    return x->def < y->def ? -1 : (x->def > y->def);
}

static int alloc_run(nv_ctx* c, unsigned n) {
    for (int r = c->first_alloc; r + (int)n <= NV_GPR_LIMIT; r++) {
        unsigned k = 0;
        while (k < n && !c->used[r + k]) k++;
        if (k < n) {
            r += (int)k;
            continue;
        }
        memset(c->used + r, 1, n);
        if (r + (int)n > c->max_reg)
            c->max_reg = r + (int)n;
        return r;
    }
    return -1;
}

/* Linear scan without spilling: a value never shares a register with one that
 * is still read by the instruction defining it. */
static void assign_registers(nv_ctx* c) {
    unsigned n = 0;
    for (unsigned i = 0; i < c->ndefs; i++)
        if (c->iv[i].ncomp)
            c->iv[n++] = c->iv[i];
    qsort(c->iv, n, sizeof(interval), iv_cmp);

    unsigned* active = malloc((n ? n : 1) * sizeof(unsigned));
    unsigned nactive = 0;
    for (unsigned i = 0; i < n && !c->failed; i++) {
        interval* iv = &c->iv[i];
        unsigned keep = 0;
        for (unsigned k = 0; k < nactive; k++) {
            interval* a = &c->iv[active[k]];
            if (a->end < iv->start)
                memset(c->used + c->def_reg[a->def], 0, a->ncomp);
            else
                active[keep++] = active[k];
        }
        nactive = keep;
        int r = alloc_run(c, iv->ncomp);
        if (r < 0) {
            fail(c, "out of GPRs (%u values live at once)", nactive);
            break;
        }
        c->def_reg[iv->def] = r;
        active[nactive++] = i;
    }
    free(active);
}

static bool scan_block_cb(nv_ctx* c, nir_block* block, bool* has_jump) {
    nir_foreach_instr(instr, block) {
        if (instr->type == nir_instr_type_jump) {
            nir_jump_type t = nir_instr_as_jump(instr)->type;
            if (t == nir_jump_break || t == nir_jump_continue)
                *has_jump = true;
        } else if (instr->type == nir_instr_type_intrinsic) {
            nir_intrinsic_instr* intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic == nir_intrinsic_load_ssbo)
                c->ssbo_used[ssbo_block(c, intr->src[0])] = 1;
            else if (intr->intrinsic == nir_intrinsic_store_ssbo)
                c->ssbo_used[ssbo_block(c, intr->src[1])] = 1;
            else if (intr->intrinsic == nir_intrinsic_ssbo_atomic)
                c->ssbo_used[ssbo_block(c, intr->src[0])] = 1;
        }
    }
    return !c->failed;
}

/* ---- top-level compile -------------------------------------------------- */
static nv_be_test_hook_fn s_test_hook;
static void* s_test_hook_user;

void nv_be_set_test_hook(nv_be_test_hook_fn fn, void* user) {
    s_test_hook = fn;
    s_test_hook_user = user;
}

/* ---- NIR peepholes ------------------------------------------------------ */
/* The shared frontend ships no generic nir_opt_algebraic, so the two rewrites
 * that matter for instruction count are done here on scalar code: x + 0 and
 * x * 1 left behind by scalarization, and multiply-add fusion. */
static bool src_const(nir_alu_instr* alu, unsigned i, uint32_t* v) {
    return def_const(alu->src[i].src.ssa, alu->src[i].swizzle[0], v);
}

static nir_alu_instr* fusable_mul(nir_alu_instr* add, unsigned i) {
    nir_instr* in = nir_def_instr(add->src[i].src.ssa);
    if (in->type != nir_instr_type_alu)
        return NULL;
    nir_alu_instr* mul = nir_instr_as_alu(in);
    if (mul->op != nir_op_fmul || mul->def.num_components != 1 ||
        mul->def.bit_size != 32 ||
        ((mul->fp_math_ctrl | add->fp_math_ctrl) & nir_fp_no_contract))
        return NULL;
    unsigned uses = 0;
    nir_foreach_use_including_if(use, &mul->def)
        uses++;
    return uses == 1 ? mul : NULL;
}

/* source i when it is a scalar `op`, else NULL */
static nir_alu_instr* src_alu(nir_alu_instr* alu, unsigned i, nir_op op) {
    nir_instr* in = nir_def_instr(alu->src[i].src.ssa);
    if (in->type != nir_instr_type_alu)
        return NULL;
    nir_alu_instr* a = nir_instr_as_alu(in);
    return a->op == op && a->def.num_components == 1 ? a : NULL;
}

static bool peephole_alu(nir_builder* b, nir_alu_instr* alu) {
    if (alu->def.num_components != 1 || alu->def.bit_size != 32 ||
        (alu->fp_math_ctrl & nir_fp_no_transform))
        return false;
    nir_def* repl = NULL;
    uint32_t k;
    b->cursor = nir_before_instr(&alu->instr);
    if ((alu->op == nir_op_fmul ||
         ((alu->op == nir_op_fadd || alu->op == nir_op_fsub) &&
          !(alu->fp_math_ctrl & nir_fp_preserve_signed_zero)))) {
        uint32_t ident = alu->op == nir_op_fmul ? 0x3f800000u : 0;
        for (unsigned i = alu->op == nir_op_fsub ? 1 : 0; i < 2 && !repl; i++)
            if (src_const(alu, i, &k) &&
                (alu->op == nir_op_fmul ? k : k << 1) == ident)
                repl = nir_ssa_for_alu_src(b, alu, 1 - i);
    }
    /* MUFU.SIN/COS take turns: expose the scaling so sin(x) and cos(x) of the
     * same angle share it through CSE */
    if (alu->op == nir_op_fsin || alu->op == nir_op_fcos) {
        nir_def* t = nir_fmul(b, nir_ssa_for_alu_src(b, alu, 0),
                              nir_imm_int(b, 0x3e22f983));   /* 1/(2*pi) */
        repl = alu->op == nir_op_fsin ? nir_fsin_normalized_2_pi(b, t)
                                      : nir_fcos_normalized_2_pi(b, t);
    }
    /* pow(x, n) arrives as exp2(n * log2(|x|)), two trips through the MUFU
     * unit: a small whole n is cheaper, and more accurate, as
     * square-and-multiply */
    nir_alu_instr* mul = alu->op == nir_op_fexp2 ? src_alu(alu, 0, nir_op_fmul) : NULL;
    for (unsigned i = 0; i < 2 && mul && !repl; i++) {
        nir_alu_instr* lg = src_alu(mul, 1 - i, nir_op_flog2);
        float y;
        if (!lg || !src_const(mul, i, &k))
            continue;
        memcpy(&y, &k, 4);
        unsigned n = y >= 1.0f && y <= 128.0f ? (unsigned)y : 0;
        if (!n || y != (float)n || 31 - __builtin_clz(n) + __builtin_popcount(n) > 9)
            continue;
        nir_def* x = nir_ssa_for_alu_src(b, lg, 0);
        nir_alu_instr* ab = src_alu(lg, 0, nir_op_fabs);
        /* squares do not need the |x| */
        nir_def* sq = ab ? nir_ssa_for_alu_src(b, ab, 0) : x;
        if (n & 1)
            repl = x;
        for (n >>= 1; n; n >>= 1) {
            sq = nir_fmul(b, sq, sq);
            if (n & 1)
                repl = repl ? nir_fmul(b, repl, sq) : sq;
        }
    }
    for (unsigned i = 0; i < 2 && !repl; i++) {
        if (alu->op != nir_op_fadd && alu->op != nir_op_fsub)
            break;
        nir_alu_instr* mul = fusable_mul(alu, i);
        if (!mul)
            continue;
        bool c0 = src_const(mul, 0, &k), c1 = src_const(mul, 1, &k);
        /* FFMA takes one immediate: a constant factor and a constant addend
         * would need a MOV, no better than FMUL + FADD */
        if ((c0 || c1) && src_const(alu, 1 - i, &k))
            continue;
        nir_def* x = nir_ssa_for_alu_src(b, mul, 0);
        nir_def* y = nir_ssa_for_alu_src(b, mul, 1);
        nir_def* z = nir_ssa_for_alu_src(b, alu, 1 - i);
        if (alu->op == nir_op_fsub) {
            if (i == 0)
                z = nir_fneg(b, z);          /* x*y - z */
            else if (c0)
                x = nir_fneg(b, x);          /* z - x*y, sign into the constant */
            else
                y = nir_fneg(b, y);
        }
        repl = nir_ffma(b, x, y, z);
    }
    if (!repl)
        return false;
    nir_def_replace(&alu->def, repl);
    return true;
}

static bool nv_peephole(nir_shader* nir) {
    bool progress = false;
    nir_foreach_function_impl(impl, nir) {
        nir_builder b = nir_builder_create(impl);
        bool p = false;
        nir_foreach_block(block, impl) {
            nir_foreach_instr_safe(instr, block) {
                if (instr->type == nir_instr_type_alu)
                    p |= peephole_alu(&b, nir_instr_as_alu(instr));
            }
        }
        progress |= nir_progress(p, impl, nir_metadata_control_flow);
    }
    return progress;
}

static void lower(nir_shader* nir) {
    nir_lower_system_values(nir);
    nir_lower_compute_system_values(nir, NULL);
    nir_lower_alu_to_scalar(nir, NULL, NULL);
    nir_lower_load_const_to_scalar(nir);
    nir_lower_phis_to_scalar(nir, NULL, NULL);
    for (int i = 0; i < 16; i++) {
        bool progress = false;
        progress |= nir_opt_copy_prop(nir);
        progress |= nir_opt_remove_phis(nir);
        progress |= nir_opt_dce(nir);
        progress |= nir_opt_dead_cf(nir);
        progress |= nir_opt_cse(nir);
        progress |= nir_opt_constant_folding(nir);
        progress |= nv_peephole(nir);
        if (!progress)
            break;
    }
    /* last: the passes above expect 1-bit booleans */
    nir_lower_bool_to_int32(nir);
    for (int i = 0; i < 8; i++) {
        bool progress = false;
        progress |= nir_opt_copy_prop(nir);
        progress |= nir_opt_dce(nir);
        progress |= nir_opt_cse(nir);
        progress |= nir_opt_constant_folding(nir);
        if (!progress)
            break;
    }
}

static void compile_impl(nv_ctx* c, nir_function_impl* impl) {
    uint32_t w[4];
    c->impl = impl;
    nir_metadata_require(impl, nir_metadata_control_flow);
    nir_index_blocks(impl);
    nir_index_ssa_defs(impl);
    c->ndefs = impl->ssa_alloc;

    c->block_end = calloc(impl->num_blocks + 1, sizeof(uint32_t));
    c->iv = calloc(c->ndefs ? c->ndefs : 1, sizeof(interval));
    c->def_reg = malloc((c->ndefs ? c->ndefs : 1) * sizeof(int));
    c->fold = calloc(c->ndefs ? c->ndefs : 1, 1);
    mark_folds(c, impl);
    for (unsigned i = 0; i < c->ndefs; i++)
        c->def_reg[i] = -1;

    bool has_jump = false;
    nir_foreach_block(block, impl) {
        if (!scan_block_cb(c, block, &has_jump))
            return;
    }

    /* count loops, then number for real */
    number_list(c, &impl->body);
    unsigned nloops = c->nloops;
    c->loop_start = calloc(nloops ? nloops : 1, sizeof(uint32_t));
    c->loop_end = calloc(nloops ? nloops : 1, sizeof(uint32_t));
    c->pos = 0;
    c->nloops = 0;
    number_list(c, &impl->body);
    liveness_list(c, &impl->body);
    if (c->failed)
        return;

    c->esc_reg = has_jump ? NV_NUM_SCRATCH : -1;
    c->first_alloc = NV_NUM_SCRATCH + (has_jump ? 1 : 0);
    c->max_reg = c->first_alloc;
    assign_registers(c);
    if (c->failed)
        return;

    if (has_jump)
        e_mov(c, c->esc_reg, nv_imm(0xffffffffu));
    for (unsigned b = 0; b < NV_MAX_SSBO; b++) {
        if (!c->ssbo_used[b])
            continue;
        nv_uldc(w, NV_GLOBAL_UR, 0, NV_CB0_GLOBAL_BASE, NV_MT_B64, 1, ct_alu());
        emit(c, w);
        break;
    }
    emit_cf_list(c, &impl->body);
    nv_exit(w, ct_branch());
    emit(c, w);
}

bool nv_be_compile_compute(nir_shader* cs, struct nv_be_result* res,
                           uint32_t tex_ssbo_base, char* log, size_t logsz) {
    if (!cs || !res) return false;
    memset(res, 0, sizeof(*res));

    nir_shader* nir = nir_shader_clone(NULL, cs);
    if (!nv_lower_tex(nir, &res->tex_mask, tex_ssbo_base, log, logsz)) {
        ralloc_free(nir);
        return false;
    }
    lower(nir);

    nv_ctx* c = calloc(1, sizeof(*c));
    nv_asm* asmb = malloc(sizeof(*asmb));
    nv_asm_init(asmb);
    c->out = asmb;

    unsigned nimpl = 0;
    nir_foreach_function_impl(impl, nir) {
        if (nimpl++ == 0)
            compile_impl(c, impl);
        else
            fail(c, "more than one function survived inlining");
    }
    if (nimpl == 0)
        fail(c, "shader has no entry point");
    if (!c->failed && nv_asm_finish(asmb) != 0)
        fail(c, "assembler: unbound label or table overflow (%u instructions)",
             asmb->n);

    bool ok = !c->failed;
    if (ok) {
        res->bin_words = asmb->n * 4;
        res->bin = malloc(res->bin_words * sizeof(uint32_t));
        memcpy(res->bin, asmb->w, res->bin_words * sizeof(uint32_t));
        res->meta.num_gprs = (uint32_t)c->max_reg + 2;
        res->meta.local_size[0] = nir->info.workgroup_size[0];
        res->meta.local_size[1] = nir->info.workgroup_size[1];
        res->meta.local_size[2] = nir->info.workgroup_size[2];
        if (s_test_hook)
            s_test_hook(nir, res, s_test_hook_user);
    } else if (log) {
        snprintf(log, logsz, "%s", c->log);
    }

    free(c->block_end);
    free(c->loop_start);
    free(c->loop_end);
    free(c->iv);
    free(c->def_reg);
    free(c->fold);
    free(c);
    free(asmb);
    ralloc_free(nir);
    return ok;
}

void nv_be_result_free(struct nv_be_result* res) {
    if (!res) return;
    free(res->bin);
    memset(res, 0, sizeof(*res));
}
