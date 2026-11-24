/*
 * nv_sass.h - NVIDIA sm_87 (GA10B / Ampere) SASS instruction encoder.
 *
 * Pure C99, no platform dependencies: builds on the host for golden tests and
 * cross for the EwokOS target alike.  Each instruction is 128 bits = 4 u32
 * words (little-endian).  Bit assignments are transcribed from Mesa NAK's
 * src/nouveau/compiler/nak/sm70_encode.rs (ShaderModel70 covers sm_70..sm_87)
 * and verified bit-for-bit against nvcc -arch=sm_87 + nvdisasm golden output.
 *
 * Layout (sm_70+, per-instruction control, NOT the sm_50/60 grouped control):
 *   [0:12]   opcode (ALU ops split [0:9]=opcode, [9:12]=operand form)
 *   [12:15]  guard predicate idx, [15] invert
 *   [16:24]  GPR dst (255 = RZ / none)
 *   [105:109] stall/delay (0..15)
 *   [109]     yield
 *   [110:113] write barrier idx (7 = none)
 *   [113:116] read barrier idx (7 = none)
 *   [116:122] wait barrier mask (6 bits)
 *   [122:126] reuse flags (4 bits)
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NV_SASS_H
#define NV_SASS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- register files ----------------------------------------------------- */
/* GPR 0..254, RZ = 255.  UGPR 0..62, URZ = 63.  Pred 0..6, PT = 7.
 * UPred 0..6, UPT = 7.  Bar 0..15. */
#define NV_RZ   255
#define NV_URZ  63
#define NV_PT   7
#define NV_UPT  7

/* ---- source operands ---------------------------------------------------- */
enum nv_src_kind {
    NV_SRC_NONE = 0,   /* absent operand: field left 0 (NAK SrcRef None) */
    NV_SRC_ZERO,       /* RZ / URZ (encoded as 255 / 63) */
    NV_SRC_GPR,        /* regular GPR */
    NV_SRC_UGPR,       /* uniform GPR */
    NV_SRC_IMM,        /* 32-bit immediate */
    NV_SRC_CB          /* constant buffer c[buf][off] */
};

typedef struct {
    uint8_t  kind;     /* enum nv_src_kind */
    uint8_t  reg;      /* GPR / UGPR index */
    uint8_t  comps;    /* 1 = 32-bit, 2 = 64-bit register pair */
    uint8_t  neg;      /* integer/float negate modifier */
    uint8_t  abs;      /* float abs modifier */
    uint32_t imm;      /* NV_SRC_IMM */
    uint8_t  cb_buf;   /* NV_SRC_CB: constant bank index */
    uint32_t cb_off;   /* NV_SRC_CB: byte offset */
} nv_src;

static inline nv_src nv_none(void)        { nv_src s = {0}; s.kind = NV_SRC_NONE; return s; }
static inline nv_src nv_zero(void)        { nv_src s = {0}; s.kind = NV_SRC_ZERO; return s; }
static inline nv_src nv_gpr(uint8_t r)    { nv_src s = {0}; s.kind = NV_SRC_GPR;  s.reg = r; s.comps = 1; return s; }
static inline nv_src nv_gpr64(uint8_t r)  { nv_src s = {0}; s.kind = NV_SRC_GPR;  s.reg = r; s.comps = 2; return s; }
static inline nv_src nv_ugpr(uint8_t r)   { nv_src s = {0}; s.kind = NV_SRC_UGPR; s.reg = r; s.comps = 1; return s; }
static inline nv_src nv_ugpr64(uint8_t r) { nv_src s = {0}; s.kind = NV_SRC_UGPR; s.reg = r; s.comps = 2; return s; }
static inline nv_src nv_imm(uint32_t v)   { nv_src s = {0}; s.kind = NV_SRC_IMM;  s.imm = v; return s; }
static inline nv_src nv_cb(uint8_t b, uint32_t off) {
    nv_src s = {0}; s.kind = NV_SRC_CB; s.cb_buf = b; s.cb_off = off; return s;
}

/* ---- predicate guard / source ------------------------------------------ */
/* A guarded instruction: @P / @!P.  PT (7) = always. */
typedef struct {
    uint8_t idx;       /* 0..7, 7 = PT */
    uint8_t inv;       /* negate */
} nv_pred;

static inline nv_pred nv_pt(void)      { nv_pred p = { NV_PT, 0 }; return p; }
static inline nv_pred nv_p(uint8_t i)  { nv_pred p = { i, 0 }; return p; }
static inline nv_pred nv_pn(uint8_t i) { nv_pred p = { i, 1 }; return p; }

/* ---- scheduling control ------------------------------------------------- */
typedef struct {
    uint8_t delay;     /* stall cycles 0..15 */
    uint8_t yld;       /* yield hint */
    uint8_t wr_bar;    /* write barrier 0..6, 7 = none */
    uint8_t rd_bar;    /* read barrier 0..6, 7 = none */
    uint8_t wt_mask;   /* wait barrier mask, 6 bits */
    uint8_t reuse;     /* reuse flags, 4 bits */
} nv_ctrl;

/* no barriers, no stall: caller fills delay for correctness */
static inline nv_ctrl nv_ctrl_none(void) {
    nv_ctrl c = {0}; c.wr_bar = 7; c.rd_bar = 7; return c;
}

/* ---- float rounding / compare ops -------------------------------------- */
enum nv_rnd { NV_RND_NEAREST = 0, NV_RND_NEGINF = 1, NV_RND_POSINF = 2, NV_RND_ZERO = 3 };
enum nvicmp { NV_ICMP_FALSE=0, NV_ICMP_LT=1, NV_ICMP_EQ=2, NV_ICMP_LE=3,
              NV_ICMP_GT=4, NV_ICMP_NE=5, NV_ICMP_GE=6, NV_ICMP_TRUE=7 };
enum nvfcmp { NV_FCMP_OLT=1, NV_FCMP_OEQ=2, NV_FCMP_OLE=3, NV_FCMP_OGT=4,
              NV_FCMP_ONE=5, NV_FCMP_OGE=6, NV_FCMP_NUM=7, NV_FCMP_NAN=8,
              NV_FCMP_LT=9, NV_FCMP_EQ=10, NV_FCMP_LE=11, NV_FCMP_GT=12,
              NV_FCMP_NE=13, NV_FCMP_GE=14 };
enum nvsetop { NV_SET_AND=0, NV_SET_OR=1, NV_SET_XOR=2 };
enum nvmt { NV_MT_U8=0, NV_MT_I8=1, NV_MT_U16=2, NV_MT_I16=3,
            NV_MT_B32=4, NV_MT_B64=5, NV_MT_B128=6 };
/* memory order (sm_80+ encoding, [77:81]) */
enum nvorder { NV_ORD_WEAK=0x0, NV_ORD_CONSTANT=0x4, NV_ORD_CTA=0x5,
               NV_ORD_GPU=0x7, NV_ORD_SYS=0xa };
enum nvevict { NV_EV_FIRST=0, NV_EV_NORMAL=1, NV_EV_LAST=2, NV_EV_LASTUSE=3,
               NV_EV_UNCHANGED=4, NV_EV_NOALLOC=5 };

/* ---- special register indices (S2R / CS2R) ------------------------------ */
#define NV_SR_TID_X      0x21
#define NV_SR_TID_Y      0x22
#define NV_SR_TID_Z      0x23
#define NV_SR_CTAID_X    0x25
#define NV_SR_CTAID_Y    0x26
#define NV_SR_CTAID_Z    0x27
#define NV_SR_LANEID     0x00
#define NV_SR_EQMASK     0x08
#define NV_SR_LTMASK     0x09
#define NV_SR_LEMASK     0x0a
#define NV_SR_GEQMASK    0x0b
#define NV_SR_GTMASK     0x0c
#define NV_SR_CLOCK_LO   0x50
#define NV_SR_CLOCK_HI   0x51
#define NV_SR_GLOBALTIMER_LO 0x60
#define NV_SR_GLOBALTIMER_HI 0x61

/* ---- low-level field helpers (exposed for tests) ------------------------ */
void nv_set_field(uint32_t w[4], unsigned lo, unsigned hi, uint64_t val);
uint64_t nv_get_field(const uint32_t w[4], unsigned lo, unsigned hi);
void nv_set_ctrl(uint32_t w[4], nv_ctrl c);
void nv_set_guard(uint32_t w[4], nv_pred p);

/* ---- instruction encoders ---------------------------------------------- *
 * Each writes a 128-bit instruction into out[4].  `dst` is a GPR index
 * (NV_RZ for none).  Control is applied last.  These mirror NAK's per-op
 * encode() bodies; operand-form rules follow encode_alu_base(). */

/* MOV / UMOV */
void nv_mov(uint32_t out[4], uint8_t dst, nv_src src, nv_ctrl c);
void nv_umov(uint32_t out[4], uint8_t udst, nv_src src, nv_ctrl c);

/* float ALU */
void nv_fadd(uint32_t out[4], uint8_t dst, nv_src a, nv_src b,
             int sat, int ftz, enum nv_rnd rnd, nv_ctrl c);
void nv_fmul(uint32_t out[4], uint8_t dst, nv_src a, nv_src b,
             int sat, int ftz, enum nv_rnd rnd, nv_ctrl c);
void nv_ffma(uint32_t out[4], uint8_t dst, nv_src a, nv_src b, nv_src cc,
             int sat, int ftz, enum nv_rnd rnd, nv_ctrl c);
void nv_fmnmx(uint32_t out[4], uint8_t dst, nv_src a, nv_src b,
              nv_pred min_pred, int ftz, nv_ctrl c);
void nv_fsetp(uint32_t out[4], uint8_t pdst, nv_src a, nv_src b,
              enum nvfcmp cmp, enum nvsetop setop, nv_pred accum,
              int ftz, nv_ctrl c);

/* integer ALU */
void nv_imad(uint32_t out[4], uint8_t dst, nv_src a, nv_src b, nv_src cc,
             int is_signed, nv_ctrl c);
void nv_imad_wide(uint32_t out[4], uint8_t dst, nv_src a, nv_src b, nv_src cc,
                  int is_signed, nv_ctrl c);
void nv_iadd3(uint32_t out[4], uint8_t dst, nv_src a, nv_src b, nv_src cc,
              nv_ctrl c);
void nv_imnmx(uint32_t out[4], uint8_t dst, nv_src a, nv_src b,
              nv_pred min_pred, int is_signed, nv_ctrl c);
void nv_isetp(uint32_t out[4], uint8_t pdst, nv_src a, nv_src b,
              enum nvicmp cmp, enum nvsetop setop, nv_pred accum,
              int is_signed, int ex, nv_ctrl c);
void nv_lop3(uint32_t out[4], uint8_t dst, nv_src a, nv_src b, nv_src cc,
             uint8_t lut, nv_pred pdst, nv_ctrl c);
/* dst_high selects the upper half of the (hi:lo) funnel result: NIR 32-bit
 * right shifts are SHF.R with lo=RZ, hi=x, dst_high=1 (NAK builder shr). */
void nv_shf(uint32_t out[4], uint8_t dst, nv_src lo, nv_src hi, nv_src shift,
            int right, int wrap, int is_64, int is_signed, int dst_high,
            nv_ctrl c);
void nv_sel(uint32_t out[4], uint8_t dst, nv_src a, nv_src b, nv_pred sel,
            nv_ctrl c);
void nv_mufu(uint32_t out[4], uint8_t dst, nv_src src, uint8_t op, nv_ctrl c);
/* MUFU ops */
#define NV_MUFU_COS 0
#define NV_MUFU_SIN 1
#define NV_MUFU_EX2 2
#define NV_MUFU_LG2 3
#define NV_MUFU_RCP 4
#define NV_MUFU_RSQ 5
#define NV_MUFU_SQRT 8

/* conversions.  *_bits are 16/32/64; size fields encode ilog2(bits/8). */
void nv_f2f(uint32_t out[4], uint8_t dst, nv_src src, int dst_bits,
            int src_bits, enum nv_rnd rnd, int ftz, nv_ctrl c);
void nv_f2i(uint32_t out[4], uint8_t dst, nv_src src, int is_signed,
            int dst_bits, int src_bits, enum nv_rnd rnd, int ftz, nv_ctrl c);
void nv_i2f(uint32_t out[4], uint8_t dst, nv_src src, int is_signed,
            int dst_bits, int src_bits, enum nv_rnd rnd, nv_ctrl c);
/* FRND: round a float to an integral float (floor/ceil/trunc/round-even) */
void nv_frnd(uint32_t out[4], uint8_t dst, nv_src src, int bits,
             enum nv_rnd rnd, int ftz, nv_ctrl c);

/* memory: global load/store (LDG.E / STG.E).  addr is a 64-bit GPR pair when
 * addr64 != 0; uaddr is the uniform base register (URZ for none, treated as
 * 64-bit per NAK); offset is a signed byte immediate. */
void nv_ldg(uint32_t out[4], uint8_t dst, nv_src addr, int addr64, nv_src uaddr,
            int32_t offset, enum nvmt mt, enum nvorder order,
            enum nvevict ev, int dst64, nv_ctrl c);
void nv_stg(uint32_t out[4], nv_src addr, int addr64, nv_src uaddr, nv_src data,
            int32_t offset, enum nvmt mt, enum nvorder order,
            enum nvevict ev, nv_ctrl c);
/* shared / local load/store (LDS/STS, LDL/STL) */
void nv_lds(uint32_t out[4], uint8_t dst, nv_src addr, int32_t offset,
            enum nvmt mt, nv_ctrl c);
void nv_sts(uint32_t out[4], nv_src addr, nv_src data, int32_t offset,
            enum nvmt mt, nv_ctrl c);
/* constant load: LDC (per-thread) / ULDC (uniform) */
void nv_ldc(uint32_t out[4], uint8_t dst, nv_src offset, uint8_t cbuf,
            uint32_t cb_off, enum nvmt mt, nv_ctrl c);
void nv_uldc(uint32_t out[4], uint8_t udst, uint8_t cbuf, uint32_t cb_off,
             enum nvmt mt, int dst64, nv_ctrl c);

/* control flow */
void nv_bra(uint32_t out[4], int32_t rel_dwords, nv_pred cond, nv_ctrl c);
void nv_exit(uint32_t out[4], nv_ctrl c);
void nv_nop(uint32_t out[4], nv_ctrl c);
void nv_bar(uint32_t out[4], nv_ctrl c);          /* BAR.SYNC */
void nv_membar(uint32_t out[4], int scope, nv_ctrl c);
void nv_bmov(uint32_t out[4], uint8_t bar, nv_src src, int to_bar, int clear,
             nv_ctrl c);
/* BMOV.32.CLEAR RZ, Bn: zero a branch-sync barrier (NAK OpBClear) */
void nv_bclear(uint32_t out[4], uint8_t bar, nv_ctrl c);
void nv_bsync(uint32_t out[4], uint8_t bar, nv_pred cond, nv_ctrl c);
void nv_break(uint32_t out[4], uint8_t bar, int32_t rel_dwords, nv_pred cond,
              nv_ctrl c);
void nv_bssy(uint32_t out[4], uint8_t bar, int32_t rel_dwords, nv_pred cond,
             nv_ctrl c);
void nv_warpsync(uint32_t out[4], uint32_t mask, nv_ctrl c);

/* special-register moves */
void nv_s2r(uint32_t out[4], uint8_t dst, uint8_t idx, nv_ctrl c);
void nv_cs2r(uint32_t out[4], uint8_t dst, uint8_t idx, int dst64, nv_ctrl c);
void nv_r2ur(uint32_t out[4], uint8_t udst, nv_src src, nv_ctrl c);

/* ---- assembler ---------------------------------------------------------- *
 * A tiny label-resolving assembler: append instructions, mark labels, patch
 * branches.  Branch offsets are in dword (4-byte) units relative to the next
 * instruction (target - ip - 4 in NAK's dword-addressed ip). */
#define NV_ASM_MAX_INSTR 16384
#define NV_ASM_MAX_PATCH 4096
#define NV_ASM_MAX_LABEL 4096

typedef struct {
    uint32_t w[NV_ASM_MAX_INSTR][4];
    unsigned n;
    /* branch patch records: instr index, field lo/hi, label id */
    struct { unsigned instr; unsigned lo; unsigned hi; int label; }
        patch[NV_ASM_MAX_PATCH];
    unsigned npatch;
    int label_at[NV_ASM_MAX_LABEL];   /* dword address of each label, -1 unset */
    unsigned nlabel;
    int overflow;                     /* a table filled up: finish() fails */
} nv_asm;

void nv_asm_init(nv_asm* a);
unsigned nv_asm_emit(nv_asm* a, const uint32_t w[4]);   /* returns instr index */
int nv_asm_label(nv_asm* a);                            /* returns label id */
void nv_asm_bind(nv_asm* a, int label);                 /* bind here */
/* emit a BRA whose target is `label`; patched at finish */
void nv_asm_bra(nv_asm* a, int label, nv_pred cond, nv_ctrl c);
void nv_asm_break(nv_asm* a, uint8_t bar, int label, nv_pred cond, nv_ctrl c);
void nv_asm_bssy(nv_asm* a, uint8_t bar, int label, nv_pred cond, nv_ctrl c);
/* resolve patches; returns 0 on success, -1 on unbound label / overflow */
int nv_asm_finish(nv_asm* a);
/* total bytes of assembled code */
static inline unsigned nv_asm_bytes(const nv_asm* a) { return a->n * 16; }

#ifdef __cplusplus
}
#endif

#endif /* NV_SASS_H */