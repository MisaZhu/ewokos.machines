/*
 * nv_sass.c - sm_87 SASS encoder.  See nv_sass.h for the layout contract.
 * Transcribed from Mesa NAK sm70_encode.rs (ShaderModel70, sm_70..sm_87).
 * SPDX-License-Identifier: MIT
 */
#include "nv_sass.h"
#include <string.h>

/* ---- bit field helpers -------------------------------------------------- */
void nv_set_field(uint32_t w[4], unsigned lo, unsigned hi, uint64_t val) {
    unsigned n = hi - lo;
    uint64_t mask = (n >= 64) ? ~0ull : ((1ull << n) - 1);
    val &= mask;
    while (n) {
        unsigned word = lo / 32;
        unsigned bit = lo % 32;
        unsigned take = 32 - bit;
        if (take > n) take = n;
        uint32_t m = (uint32_t)(((1ull << take) - 1) << bit);
        w[word] = (w[word] & ~m) | ((uint32_t)(val << bit) & m);
        val >>= take;
        lo += take;
        n -= take;
    }
}

uint64_t nv_get_field(const uint32_t w[4], unsigned lo, unsigned hi) {
    unsigned n = hi - lo;
    uint64_t val = 0;
    unsigned got = 0;
    while (n) {
        unsigned word = lo / 32;
        unsigned bit = lo % 32;
        unsigned take = 32 - bit;
        if (take > n) take = n;
        uint64_t chunk = (w[word] >> bit) & ((1ull << take) - 1);
        val |= chunk << got;
        got += take;
        lo += take;
        n -= take;
    }
    return val;
}

void nv_set_ctrl(uint32_t w[4], nv_ctrl c) {
    nv_set_field(w, 105, 109, c.delay);
    nv_set_field(w, 110, 113, c.wr_bar);
    nv_set_field(w, 113, 116, c.rd_bar);
    nv_set_field(w, 116, 122, c.wt_mask);
    nv_set_field(w, 109, 110, c.yld);
    nv_set_field(w, 122, 126, c.reuse);
}

void nv_set_guard(uint32_t w[4], nv_pred p) {
    nv_set_field(w, 12, 15, p.idx);
    nv_set_field(w, 15, 16, p.inv);
}

static void set_dst(uint32_t w[4], uint8_t dst) {
    nv_set_field(w, 16, 24, dst);
}

static void set_pred_dst(uint32_t w[4], unsigned lo, uint8_t p) {
    nv_set_field(w, lo, lo + 3, p);
}

/* predicate source: idx [lo:lo+3], invert [not_bit] */
static void set_pred_src(uint32_t w[4], unsigned lo, unsigned not_bit,
                         nv_pred p) {
    nv_set_field(w, lo, lo + 3, p.idx);
    nv_set_field(w, not_bit, not_bit + 1, p.inv);
}

static void set_src_cb(uint32_t w[4], unsigned lo, unsigned cx_bit,
                       uint8_t buf, uint32_t off) {
    nv_set_field(w, lo + 6, lo + 22, off);
    nv_set_field(w, lo + 22, lo + 27, buf);
    nv_set_field(w, cx_bit, cx_bit + 1, 0);
}

/* ---- ALU source encoding (encode_alu_base) ------------------------------ */
static void alu_src0(uint32_t w[4], nv_src s) {
    if (s.kind == NV_SRC_NONE) return;   /* field left 0 */
    if (s.kind == NV_SRC_ZERO) {
        nv_set_field(w, 24, 32, NV_RZ);
    } else if (s.kind == NV_SRC_GPR) {
        nv_set_field(w, 24, 32, s.reg);
    } else if (s.kind == NV_SRC_UGPR) {
        nv_set_field(w, 24, 30, s.reg);
    }
    nv_set_field(w, 72, 73, s.neg);
    nv_set_field(w, 73, 74, s.abs);
}

static void alu_src1_reg(uint32_t w[4], nv_src s) {
    uint8_t r = (s.kind == NV_SRC_ZERO) ? NV_RZ : s.reg;
    nv_set_field(w, 32, 40, r);
    nv_set_field(w, 62, 63, s.abs);
    nv_set_field(w, 63, 64, s.neg);
}

static void alu_src1_ureg(uint32_t w[4], nv_src s) {
    uint8_t r = (s.kind == NV_SRC_ZERO) ? NV_URZ : s.reg;
    nv_set_field(w, 32, 38, r);
    nv_set_field(w, 62, 63, s.abs);
    nv_set_field(w, 63, 64, s.neg);
    nv_set_field(w, 91, 92, 1);
}

static void alu_src1_imm(uint32_t w[4], uint32_t imm) {
    nv_set_field(w, 32, 64, imm);
}

static void alu_src1_cb(uint32_t w[4], nv_src s) {
    set_src_cb(w, 32, 91, s.cb_buf, s.cb_off);
    nv_set_field(w, 62, 63, s.abs);
    nv_set_field(w, 63, 64, s.neg);
}

static void alu_src2_reg(uint32_t w[4], nv_src s) {
    uint8_t r = (s.kind == NV_SRC_ZERO) ? NV_RZ : s.reg;
    nv_set_field(w, 64, 72, r);
    nv_set_field(w, 74, 75, s.abs);
    nv_set_field(w, 75, 76, s.neg);
}

/* Generic ALU: opcode [0:9], form [9:12].  Mirrors encode_alu_base. */
static void encode_alu(uint32_t w[4], uint16_t opcode, uint8_t dst,
                       nv_src s0, nv_src s1, nv_src s2, int has_s1, int has_s2) {
    set_dst(w, dst);
    alu_src0(w, s0);

    uint8_t form;
    if (!has_s2 || s2.kind == NV_SRC_GPR || s2.kind == NV_SRC_ZERO) {
        if (has_s2) alu_src2_reg(w, s2);
        if (!has_s1) {
            form = 1;
        } else if (s1.kind == NV_SRC_GPR || s1.kind == NV_SRC_ZERO) {
            alu_src1_reg(w, s1); form = 1;
        } else if (s1.kind == NV_SRC_UGPR) {
            alu_src1_ureg(w, s1); form = 6;
        } else if (s1.kind == NV_SRC_IMM) {
            alu_src1_imm(w, s1.imm); form = 4;
        } else {
            alu_src1_cb(w, s1); form = 5;
        }
    } else if (s2.kind == NV_SRC_UGPR) {
        nv_set_field(w, 32, 38, s2.reg);
        nv_set_field(w, 62, 63, s2.abs);
        nv_set_field(w, 63, 64, s2.neg);
        nv_set_field(w, 91, 92, 1);
        alu_src2_reg(w, s1);
        form = 7;
    } else if (s2.kind == NV_SRC_IMM) {
        alu_src1_imm(w, s2.imm);
        alu_src2_reg(w, s1);
        form = 2;
    } else {
        alu_src1_cb(w, s2);
        alu_src2_reg(w, s1);
        form = 3;
    }
    nv_set_field(w, 0, 9, opcode);
    nv_set_field(w, 9, 12, form);
}

static void finish(uint32_t out[4], nv_ctrl c) {
    nv_set_guard(out, nv_pt());
    nv_set_ctrl(out, c);
}

/* ---- MOV ---------------------------------------------------------------- */
void nv_mov(uint32_t out[4], uint8_t dst, nv_src src, nv_ctrl c) {
    memset(out, 0, 16);
    encode_alu(out, 0x002, dst, nv_none(), src, nv_none(), 1, 0);
    nv_set_field(out, 72, 76, 0xf);   /* quad_lanes = all lanes */
    finish(out, c);
}

void nv_umov(uint32_t out[4], uint8_t udst, nv_src src, nv_ctrl c) {
    memset(out, 0, 16);
    nv_set_field(out, 0, 12, 0xc82);
    nv_set_field(out, 16, 22, udst);
    uint8_t form;
    if (src.kind == NV_SRC_IMM) {
        alu_src1_imm(out, src.imm); form = 4;
    } else {
        uint8_t r = (src.kind == NV_SRC_ZERO) ? NV_URZ : src.reg;
        nv_set_field(out, 32, 38, r);
        nv_set_field(out, 62, 63, src.abs);
        nv_set_field(out, 63, 64, src.neg);
        nv_set_field(out, 91, 92, 1);
        form = 6;
    }
    nv_set_field(out, 9, 12, form);
    finish(out, c);
}

/* ---- float ALU ---------------------------------------------------------- */
void nv_fadd(uint32_t out[4], uint8_t dst, nv_src a, nv_src b,
             int sat, int ftz, enum nv_rnd rnd, nv_ctrl c) {
    memset(out, 0, 16);
    if (b.kind == NV_SRC_ZERO || b.kind == NV_SRC_GPR)
        encode_alu(out, 0x021, dst, a, b, nv_none(), 1, 0);
    else
        encode_alu(out, 0x021, dst, a, nv_zero(), b, 1, 1);
    nv_set_field(out, 77, 78, sat);
    nv_set_field(out, 78, 80, rnd);
    nv_set_field(out, 80, 81, ftz);
    finish(out, c);
}

void nv_fmul(uint32_t out[4], uint8_t dst, nv_src a, nv_src b,
             int sat, int ftz, enum nv_rnd rnd, nv_ctrl c) {
    memset(out, 0, 16);
    /* src1 carries the immediate/cbuf form (form 4/5), src2 is always RZ */
    encode_alu(out, 0x020, dst, a, b, nv_zero(), 1, 1);
    nv_set_field(out, 76, 77, 0);   /* dnz */
    nv_set_field(out, 77, 78, sat);
    nv_set_field(out, 78, 80, rnd);
    nv_set_field(out, 80, 81, ftz);
    nv_set_field(out, 84, 87, 0x4); /* PDIV */
    finish(out, c);
}

void nv_ffma(uint32_t out[4], uint8_t dst, nv_src a, nv_src b, nv_src cc,
             int sat, int ftz, enum nv_rnd rnd, nv_ctrl c) {
    memset(out, 0, 16);
    encode_alu(out, 0x023, dst, a, b, cc, 1, 1);
    nv_set_field(out, 76, 77, 0);   /* dnz */
    nv_set_field(out, 77, 78, sat);
    nv_set_field(out, 78, 80, rnd);
    nv_set_field(out, 80, 81, ftz);
    finish(out, c);
}

void nv_fmnmx(uint32_t out[4], uint8_t dst, nv_src a, nv_src b,
              nv_pred min_pred, int ftz, nv_ctrl c) {
    memset(out, 0, 16);
    encode_alu(out, 0x009, dst, a, b, nv_none(), 1, 0);
    set_pred_src(out, 87, 90, min_pred);
    nv_set_field(out, 80, 81, ftz);
    nv_set_field(out, 65, 66, 0);
    finish(out, c);
}

void nv_fsetp(uint32_t out[4], uint8_t pdst, nv_src a, nv_src b,
              enum nvfcmp cmp, enum nvsetop setop, nv_pred accum,
              int ftz, nv_ctrl c) {
    memset(out, 0, 16);
    encode_alu(out, 0x00b, NV_RZ, a, b, nv_none(), 1, 0);
    set_pred_dst(out, 81, pdst);
    set_pred_dst(out, 84, NV_PT);
    set_pred_src(out, 87, 90, accum);
    nv_set_field(out, 74, 76, setop);
    nv_set_field(out, 76, 80, cmp);
    nv_set_field(out, 80, 81, ftz);
    finish(out, c);
}

/* ---- integer ALU -------------------------------------------------------- */
void nv_imad(uint32_t out[4], uint8_t dst, nv_src a, nv_src b, nv_src cc,
             int is_signed, nv_ctrl c) {
    memset(out, 0, 16);
    encode_alu(out, 0x024, dst, a, b, cc, 1, 1);
    set_pred_dst(out, 81, NV_PT);
    set_pred_src(out, 87, 90, nv_pn(NV_PT));   /* carry-in = false */
    nv_set_field(out, 73, 74, is_signed);
    finish(out, c);
}

void nv_imad_wide(uint32_t out[4], uint8_t dst, nv_src a, nv_src b, nv_src cc,
                  int is_signed, nv_ctrl c) {
    memset(out, 0, 16);
    encode_alu(out, 0x025, dst, a, b, cc, 1, 1);
    set_pred_dst(out, 81, NV_PT);
    set_pred_src(out, 87, 90, nv_pn(NV_PT));   /* carry-in = false */
    nv_set_field(out, 73, 74, is_signed);
    finish(out, c);
}

void nv_iadd3(uint32_t out[4], uint8_t dst, nv_src a, nv_src b, nv_src cc,
              nv_ctrl c) {
    memset(out, 0, 16);
    encode_alu(out, 0x010, dst, a, b, cc, 1, 1);
    set_pred_src(out, 87, 90, nv_pn(NV_PT));   /* false */
    set_pred_src(out, 77, 80, nv_pn(NV_PT));   /* false */
    set_pred_dst(out, 81, NV_PT);   /* overflow0 none */
    set_pred_dst(out, 84, NV_PT);   /* overflow1 none */
    finish(out, c);
}

void nv_imnmx(uint32_t out[4], uint8_t dst, nv_src a, nv_src b,
              nv_pred min_pred, int is_signed, nv_ctrl c) {
    memset(out, 0, 16);
    encode_alu(out, 0x017, dst, a, b, nv_none(), 1, 0);
    set_pred_src(out, 87, 90, min_pred);
    nv_set_field(out, 73, 74, is_signed);
    finish(out, c);
}

void nv_isetp(uint32_t out[4], uint8_t pdst, nv_src a, nv_src b,
              enum nvicmp cmp, enum nvsetop setop, nv_pred accum,
              int is_signed, int ex, nv_ctrl c) {
    memset(out, 0, 16);
    encode_alu(out, 0x00c, NV_RZ, a, b, nv_none(), 1, 0);
    set_pred_src(out, 68, 71, nv_pt());   /* low_cmp = PT */
    set_pred_src(out, 87, 90, accum);
    nv_set_field(out, 72, 73, ex);
    nv_set_field(out, 73, 74, is_signed);
    nv_set_field(out, 74, 76, setop);
    nv_set_field(out, 76, 79, cmp);
    set_pred_dst(out, 81, pdst);
    set_pred_dst(out, 84, NV_PT);
    finish(out, c);
}

void nv_lop3(uint32_t out[4], uint8_t dst, nv_src a, nv_src b, nv_src cc,
             uint8_t lut, nv_pred pdst, nv_ctrl c) {
    memset(out, 0, 16);
    encode_alu(out, 0x012, dst, a, b, cc, 1, 1);
    set_pred_src(out, 87, 90, nv_pn(NV_PT));   /* false */
    nv_set_field(out, 72, 80, lut);
    nv_set_field(out, 80, 81, 0);   /* .PAND */
    set_pred_dst(out, 81, pdst.idx);
    finish(out, c);
}

void nv_shf(uint32_t out[4], uint8_t dst, nv_src lo, nv_src hi, nv_src shift,
            int right, int wrap, int is_64, int is_signed, int dst_high,
            nv_ctrl c) {
    memset(out, 0, 16);
    /* SHF: src0=lo, src1=shift, src2=hi */
    encode_alu(out, 0x019, dst, lo, shift, hi, 1, 1);
    /* data_type [73:75]: I64=0 U64=1 I32=2 U32=3 */
    uint8_t dt = is_64 ? (is_signed ? 0 : 1) : (is_signed ? 2 : 3);
    nv_set_field(out, 73, 75, dt);
    nv_set_field(out, 75, 76, wrap);
    nv_set_field(out, 76, 77, right);
    nv_set_field(out, 80, 81, dst_high);
    finish(out, c);
}

void nv_sel(uint32_t out[4], uint8_t dst, nv_src a, nv_src b, nv_pred sel,
            nv_ctrl c) {
    memset(out, 0, 16);
    encode_alu(out, 0x007, dst, a, b, nv_none(), 1, 0);
    set_pred_src(out, 87, 90, sel);
    finish(out, c);
}

void nv_mufu(uint32_t out[4], uint8_t dst, nv_src src, uint8_t op, nv_ctrl c) {
    memset(out, 0, 16);
    encode_alu(out, 0x108, dst, nv_none(), src, nv_none(), 1, 0);
    nv_set_field(out, 73, 74, 0);   /* fp16 = 0 (F32) */
    nv_set_field(out, 74, 80, op);
    finish(out, c);
}

/* ---- conversions -------------------------------------------------------- */
static uint8_t size_log2(int bits) {
    /* ilog2(bits/8): 16->1, 32->2, 64->3 */
    int bytes = bits / 8;
    uint8_t r = 0;
    while (bytes > 1) { bytes >>= 1; r++; }
    return r;
}

void nv_f2f(uint32_t out[4], uint8_t dst, nv_src src, int dst_bits,
            int src_bits, enum nv_rnd rnd, int ftz, nv_ctrl c) {
    memset(out, 0, 16);
    uint16_t op = (src_bits <= 32 && dst_bits <= 32) ? 0x104 : 0x110;
    encode_alu(out, op, dst, nv_none(), src, nv_none(), 1, 0);
    nv_set_field(out, 75, 77, size_log2(dst_bits));
    nv_set_field(out, 78, 80, rnd);
    nv_set_field(out, 80, 81, ftz);
    nv_set_field(out, 84, 86, size_log2(src_bits));
    finish(out, c);
}

void nv_f2i(uint32_t out[4], uint8_t dst, nv_src src, int is_signed,
            int dst_bits, int src_bits, enum nv_rnd rnd, int ftz, nv_ctrl c) {
    memset(out, 0, 16);
    uint16_t op = (src_bits <= 32 && dst_bits <= 32) ? 0x105 : 0x111;
    encode_alu(out, op, dst, nv_none(), src, nv_none(), 1, 0);
    nv_set_field(out, 72, 73, is_signed);
    nv_set_field(out, 75, 77, size_log2(dst_bits));
    nv_set_field(out, 77, 78, 0);   /* NTZ */
    nv_set_field(out, 78, 80, rnd);
    nv_set_field(out, 80, 81, ftz);
    nv_set_field(out, 84, 86, size_log2(src_bits));
    finish(out, c);
}

void nv_i2f(uint32_t out[4], uint8_t dst, nv_src src, int is_signed,
            int dst_bits, int src_bits, enum nv_rnd rnd, nv_ctrl c) {
    memset(out, 0, 16);
    uint16_t op = (src_bits <= 32 && dst_bits <= 32) ? 0x106 : 0x112;
    encode_alu(out, op, dst, nv_none(), src, nv_none(), 1, 0);
    nv_set_field(out, 60, 62, 0);   /* subop */
    nv_set_field(out, 74, 75, is_signed);
    nv_set_field(out, 75, 77, size_log2(dst_bits));
    nv_set_field(out, 78, 80, rnd);
    nv_set_field(out, 84, 86, size_log2(src_bits));
    finish(out, c);
}

void nv_frnd(uint32_t out[4], uint8_t dst, nv_src src, int bits,
             enum nv_rnd rnd, int ftz, nv_ctrl c) {
    memset(out, 0, 16);
    encode_alu(out, 0x107, dst, nv_none(), src, nv_none(), 1, 0);
    nv_set_field(out, 84, 86, size_log2(bits));
    nv_set_field(out, 80, 81, ftz);
    nv_set_field(out, 78, 80, rnd);
    nv_set_field(out, 75, 77, size_log2(bits));
    finish(out, c);
}

/* ---- memory ------------------------------------------------------------- */
static void set_mem_access(uint32_t w[4], enum nvmt mt, enum nvorder order,
                           enum nvevict ev) {
    nv_set_field(w, 73, 76, mt);
    nv_set_field(w, 77, 81, order);
    nv_set_field(w, 84, 87, ev);
}

void nv_ldg(uint32_t out[4], uint8_t dst, nv_src addr, int addr64, nv_src uaddr,
            int32_t offset, enum nvmt mt, enum nvorder order,
            enum nvevict ev, int dst64, nv_ctrl c) {
    memset(out, 0, 16);
    nv_set_field(out, 0, 12, 0x981);
    set_dst(out, dst);
    uint8_t ar = (addr.kind == NV_SRC_ZERO) ? NV_RZ : addr.reg;
    nv_set_field(out, 24, 32, ar);
    nv_set_field(out, 90, 91, addr64);
    /* uniform base UGPR [32:38]; zero UGPR is treated as 64-bit (size bit 1) */
    uint8_t ur = (uaddr.kind == NV_SRC_ZERO || uaddr.kind == NV_SRC_NONE)
                 ? NV_URZ : uaddr.reg;
    int u64 = (uaddr.kind == NV_SRC_ZERO || uaddr.kind == NV_SRC_NONE)
              ? 1 : (uaddr.comps == 2);
    nv_set_field(out, 32, 38, ur);
    nv_set_field(out, 72, 73, u64);
    nv_set_field(out, 64, 67, 0);   /* rev pred src = PT */
    nv_set_field(out, 67, 68, 0);
    set_pred_dst(out, 81, NV_PT);
    set_mem_access(out, mt, order, ev);
    nv_set_field(out, 76, 77, 1);   /* .E global cache-op */
    nv_set_field(out, 40, 64, (uint32_t)offset);
    nv_set_field(out, 91, 92, 1);
    (void)dst64;
    finish(out, c);
}

void nv_stg(uint32_t out[4], nv_src addr, int addr64, nv_src uaddr, nv_src data,
            int32_t offset, enum nvmt mt, enum nvorder order,
            enum nvevict ev, nv_ctrl c) {
    memset(out, 0, 16);
    nv_set_field(out, 0, 12, 0x986);
    uint8_t ar = (addr.kind == NV_SRC_ZERO) ? NV_RZ : addr.reg;
    nv_set_field(out, 24, 32, ar);
    nv_set_field(out, 90, 91, addr64);
    uint8_t dr = (data.kind == NV_SRC_ZERO) ? NV_RZ : data.reg;
    nv_set_field(out, 32, 40, dr);
    uint8_t ur = (uaddr.kind == NV_SRC_ZERO || uaddr.kind == NV_SRC_NONE)
                 ? NV_URZ : uaddr.reg;
    int u64 = (uaddr.kind == NV_SRC_ZERO || uaddr.kind == NV_SRC_NONE)
              ? 1 : (uaddr.comps == 2);
    nv_set_field(out, 64, 70, ur);
    nv_set_field(out, 72, 73, u64);
    set_mem_access(out, mt, order, ev);
    nv_set_field(out, 76, 77, 1);   /* .E global cache-op */
    nv_set_field(out, 40, 64, (uint32_t)offset);
    nv_set_field(out, 91, 92, 1);
    finish(out, c);
}

void nv_lds(uint32_t out[4], uint8_t dst, nv_src addr, int32_t offset,
            enum nvmt mt, nv_ctrl c) {
    memset(out, 0, 16);
    nv_set_field(out, 0, 12, 0x984);
    set_dst(out, dst);
    uint8_t ar = (addr.kind == NV_SRC_ZERO) ? NV_RZ : addr.reg;
    nv_set_field(out, 24, 32, ar);
    nv_set_field(out, 32, 38, NV_URZ);
    nv_set_field(out, 73, 76, mt);
    nv_set_field(out, 78, 80, 0);
    nv_set_field(out, 87, 88, 0);
    nv_set_field(out, 40, 64, (uint32_t)offset);
    nv_set_field(out, 91, 92, 1);
    finish(out, c);
}

void nv_sts(uint32_t out[4], nv_src addr, nv_src data, int32_t offset,
            enum nvmt mt, nv_ctrl c) {
    memset(out, 0, 16);
    nv_set_field(out, 0, 12, 0x988);
    uint8_t ar = (addr.kind == NV_SRC_ZERO) ? NV_RZ : addr.reg;
    nv_set_field(out, 24, 32, ar);
    uint8_t dr = (data.kind == NV_SRC_ZERO) ? NV_RZ : data.reg;
    nv_set_field(out, 32, 40, dr);
    nv_set_field(out, 64, 70, NV_URZ);
    nv_set_field(out, 73, 76, mt);
    nv_set_field(out, 78, 80, 0);
    nv_set_field(out, 40, 64, (uint32_t)offset);
    nv_set_field(out, 91, 92, 1);
    finish(out, c);
}

void nv_ldc(uint32_t out[4], uint8_t dst, nv_src offset, uint8_t cbuf,
            uint32_t cb_off, enum nvmt mt, nv_ctrl c) {
    memset(out, 0, 16);
    nv_set_field(out, 0, 12, 0xb82);
    set_dst(out, dst);
    uint8_t or_ = (offset.kind == NV_SRC_ZERO) ? NV_RZ : offset.reg;
    nv_set_field(out, 24, 32, or_);
    nv_set_field(out, 78, 80, 0);
    nv_set_field(out, 91, 92, 0);
    nv_set_field(out, 54, 59, cbuf);
    nv_set_field(out, 38, 54, cb_off);
    nv_set_field(out, 73, 76, mt);
    finish(out, c);
}

void nv_uldc(uint32_t out[4], uint8_t udst, uint8_t cbuf, uint32_t cb_off,
             enum nvmt mt, int dst64, nv_ctrl c) {
    memset(out, 0, 16);
    nv_set_field(out, 0, 12, 0xab9);
    nv_set_field(out, 91, 92, 0);
    nv_set_field(out, 16, 22, udst);
    nv_set_field(out, 54, 59, cbuf);
    nv_set_field(out, 38, 54, cb_off);
    nv_set_field(out, 73, 76, mt);
    (void)dst64;
    finish(out, c);
}

/* ---- control flow ------------------------------------------------------- */
void nv_bra(uint32_t out[4], int32_t rel_dwords, nv_pred cond, nv_ctrl c) {
    memset(out, 0, 16);
    nv_set_field(out, 0, 12, 0x947);
    nv_set_field(out, 32, 33, 0);
    set_pred_src(out, 87, 90, cond);
    nv_set_field(out, 34, 82, (uint64_t)(int64_t)rel_dwords);
    finish(out, c);
}

void nv_exit(uint32_t out[4], nv_ctrl c) {
    memset(out, 0, 16);
    nv_set_field(out, 0, 12, 0x94d);
    nv_set_field(out, 84, 85, 0);
    nv_set_field(out, 85, 86, 0);
    nv_set_field(out, 87, 90, 7);
    nv_set_field(out, 90, 91, 0);
    finish(out, c);
}

void nv_nop(uint32_t out[4], nv_ctrl c) {
    memset(out, 0, 16);
    nv_set_field(out, 0, 12, 0x918);
    finish(out, c);
}

void nv_bar(uint32_t out[4], nv_ctrl c) {
    memset(out, 0, 16);
    nv_set_field(out, 0, 12, 0xb1d);
    finish(out, c);
}

void nv_membar(uint32_t out[4], int scope, nv_ctrl c) {
    memset(out, 0, 16);
    nv_set_field(out, 0, 12, 0x992);
    nv_set_field(out, 72, 73, 0);   /* !MMIO */
    nv_set_field(out, 76, 79, scope);
    nv_set_field(out, 80, 81, 0);   /* SC */
    finish(out, c);
}

void nv_bmov(uint32_t out[4], uint8_t bar, nv_src src, int to_bar, int clear,
             nv_ctrl c) {
    memset(out, 0, 16);
    if (to_bar) {
        nv_set_field(out, 0, 12, 0x356);
        nv_set_field(out, 24, 28, bar);
        uint8_t r = (src.kind == NV_SRC_ZERO) ? NV_RZ : src.reg;
        nv_set_field(out, 32, 40, r);
    } else {
        nv_set_field(out, 0, 12, 0x355);
        set_dst(out, (src.kind == NV_SRC_ZERO) ? NV_RZ : src.reg);
        nv_set_field(out, 24, 28, bar);
    }
    nv_set_field(out, 84, 85, clear);
    finish(out, c);
}

void nv_bclear(uint32_t out[4], uint8_t bar, nv_ctrl c) {
    nv_bmov(out, bar, nv_zero(), 0, 1, c);
}

void nv_bsync(uint32_t out[4], uint8_t bar, nv_pred cond, nv_ctrl c) {
    memset(out, 0, 16);
    nv_set_field(out, 0, 12, 0x941);
    nv_set_field(out, 16, 20, bar);
    set_pred_src(out, 87, 90, cond);
    finish(out, c);
}

void nv_break(uint32_t out[4], uint8_t bar, int32_t rel_dwords, nv_pred cond,
              nv_ctrl c) {
    memset(out, 0, 16);
    nv_set_field(out, 0, 12, 0x942);
    nv_set_field(out, 16, 20, bar);
    nv_set_field(out, 34, 64, (uint64_t)(int64_t)rel_dwords);
    set_pred_src(out, 87, 90, cond);
    finish(out, c);
}

void nv_bssy(uint32_t out[4], uint8_t bar, int32_t rel_dwords, nv_pred cond,
             nv_ctrl c) {
    memset(out, 0, 16);
    nv_set_field(out, 0, 12, 0x945);
    nv_set_field(out, 16, 20, bar);
    nv_set_field(out, 34, 64, (uint64_t)(int64_t)rel_dwords);
    set_pred_src(out, 87, 90, cond);
    finish(out, c);
}

void nv_warpsync(uint32_t out[4], uint32_t mask, nv_ctrl c) {
    memset(out, 0, 16);
    encode_alu(out, 0x148, NV_RZ, nv_none(), nv_imm(mask), nv_none(), 1, 0);
    set_pred_src(out, 87, 90, nv_pt());
    finish(out, c);
}

/* ---- special registers -------------------------------------------------- */
void nv_s2r(uint32_t out[4], uint8_t dst, uint8_t idx, nv_ctrl c) {
    memset(out, 0, 16);
    nv_set_field(out, 0, 12, 0x919);
    set_dst(out, dst);
    nv_set_field(out, 72, 80, idx);
    finish(out, c);
}

void nv_cs2r(uint32_t out[4], uint8_t dst, uint8_t idx, int dst64, nv_ctrl c) {
    memset(out, 0, 16);
    nv_set_field(out, 0, 12, 0x805);
    set_dst(out, dst);
    nv_set_field(out, 72, 80, idx);
    nv_set_field(out, 80, 81, dst64);
    finish(out, c);
}

void nv_r2ur(uint32_t out[4], uint8_t udst, nv_src src, nv_ctrl c) {
    memset(out, 0, 16);
    nv_set_field(out, 0, 12, 0x3c2);   /* sm<100 (0x2ca is sm>=100) */
    nv_set_field(out, 16, 22, udst);
    uint8_t r = (src.kind == NV_SRC_ZERO) ? NV_RZ : src.reg;
    nv_set_field(out, 24, 32, r);
    set_pred_dst(out, 81, NV_PT);
    finish(out, c);
}

/* ---- assembler ---------------------------------------------------------- */
void nv_asm_init(nv_asm* a) {
    memset(a, 0, sizeof(*a));
    for (unsigned i = 0; i < NV_ASM_MAX_LABEL; i++) a->label_at[i] = -1;
}

unsigned nv_asm_emit(nv_asm* a, const uint32_t w[4]) {
    if (a->n >= NV_ASM_MAX_INSTR) {
        a->overflow = 1;
        return NV_ASM_MAX_INSTR - 1;
    }
    unsigned idx = a->n;
    memcpy(a->w[idx], w, 16);
    a->n++;
    return idx;
}

int nv_asm_label(nv_asm* a) {
    if (a->nlabel >= NV_ASM_MAX_LABEL) {
        a->overflow = 1;
        return NV_ASM_MAX_LABEL - 1;
    }
    return (int)a->nlabel++;
}

void nv_asm_bind(nv_asm* a, int label) {
    if (label < 0 || label >= NV_ASM_MAX_LABEL) {
        a->overflow = 1;
        return;
    }
    a->label_at[label] = (int)(a->n * 4);
}

static void asm_branch(nv_asm* a, int label, nv_pred cond, nv_ctrl c,
                       int kind, uint8_t bar) {
    uint32_t w[4];
    if (kind == 0) nv_bra(w, 0, cond, c);
    else if (kind == 1) nv_break(w, bar, 0, cond, c);
    else nv_bssy(w, bar, 0, cond, c);
    unsigned idx = nv_asm_emit(a, w);
    if (a->npatch >= NV_ASM_MAX_PATCH) {
        a->overflow = 1;
        return;
    }
    a->patch[a->npatch].instr = idx;
    a->patch[a->npatch].lo = 34;
    a->patch[a->npatch].hi = (kind == 0) ? 82 : 64;
    a->patch[a->npatch].label = label;
    a->npatch++;
}

void nv_asm_bra(nv_asm* a, int label, nv_pred cond, nv_ctrl c) {
    asm_branch(a, label, cond, c, 0, 0);
}
void nv_asm_break(nv_asm* a, uint8_t bar, int label, nv_pred cond, nv_ctrl c) {
    asm_branch(a, label, cond, c, 1, bar);
}
void nv_asm_bssy(nv_asm* a, uint8_t bar, int label, nv_pred cond, nv_ctrl c) {
    asm_branch(a, label, cond, c, 2, bar);
}

int nv_asm_finish(nv_asm* a) {
    if (a->overflow)
        return -1;
    for (unsigned i = 0; i < a->npatch; i++) {
        int label = a->patch[i].label;
        if (label < 0 || label >= (int)a->nlabel || a->label_at[label] < 0)
            return -1;
        int ip = (int)(a->patch[i].instr * 4);
        int target = a->label_at[label];
        int64_t rel = (int64_t)target - ip - 4;
        nv_set_field(a->w[a->patch[i].instr], a->patch[i].lo, a->patch[i].hi,
                     (uint64_t)rel);
    }
    return 0;
}