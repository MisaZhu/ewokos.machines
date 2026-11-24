/*
 * nv_emu.c - host-only sm_87 SASS warp emulator.  See nv_emu.h.
 * Field positions are decoded here independently of the nv_sass encoder so a
 * mis-encoded instruction shows up as a decode or disassembly mismatch.
 * SPDX-License-Identifier: MIT
 */
#pragma STDC FP_CONTRACT OFF
#include "nv_emu.h"
#include "nv_ref_math.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define LANES 32

enum {
    OP_MOV, OP_FADD, OP_FMUL, OP_FFMA, OP_FMNMX, OP_FSETP, OP_IMAD, OP_IMADW, OP_IADD3,
    OP_IMNMX, OP_ISETP, OP_LOP3, OP_SHF, OP_SEL, OP_MUFU, OP_F2I, OP_I2F,
    OP_FRND, OP_S2R, OP_ULDC, OP_LDG, OP_STG, OP_BRA, OP_EXIT, OP_BCLEAR,
    OP_BSYNC, OP_BSSY, OP_NOP
};

enum { K_NONE, K_REG, K_IMM, K_CB };

typedef struct {
    uint8_t kind, reg, neg, abs;
    uint32_t imm;            /* immediate, or cb0 byte offset */
} eopnd;

typedef struct {
    uint8_t op, guard, guard_inv, dst;
    uint8_t pdst, pred, pred_inv, nsrc;
    eopnd s[3];
    uint32_t a, b, c;        /* op-specific fields */
    int32_t off;
    int32_t target;
} dinstr;

enum { ST_RUN, ST_WAIT, ST_EXIT, ST_ABSENT };

typedef struct {
    uint32_t R[256];
    uint8_t Rinit[256];
    uint8_t P[8], Pinit[8];
    uint32_t pc;
    uint8_t state;
    uint32_t tid[3];
} lane_t;

struct nv_emu {
    struct nv_emu_env env;
    dinstr* prog;
    uint32_t n;
    uint64_t steps;
    lane_t L[LANES];
    uint32_t B[16];
    uint8_t Bpoison[16];
    uint64_t UR[64];
    uint8_t URinit[64];
    uint32_t ctaid[3];
    char* err;
    size_t errsz;
    int failed;
};

static uint64_t fld(const uint32_t* w, unsigned lo, unsigned hi) {
    uint64_t v = 0;
    for (unsigned i = lo; i < hi; i++)
        v |= (uint64_t)((w[i / 32] >> (i % 32)) & 1u) << (i - lo);
    return v;
}

static int64_t sext(uint64_t v, unsigned bits) {
    uint64_t m = 1ull << (bits - 1);
    return (int64_t)((v ^ m) - m);
}

static void efail(struct nv_emu* e, const char* fmt, ...) {
    if (e->failed)
        return;
    e->failed = 1;
    if (!e->err)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->err, e->errsz, fmt, ap);
    va_end(ap);
}

/* ---- decode ------------------------------------------------------------- */
static eopnd reg_at(const uint32_t* w, unsigned lo, unsigned abs_bit,
                    unsigned neg_bit) {
    eopnd o = { K_REG, (uint8_t)fld(w, lo, lo + 8),
                (uint8_t)fld(w, neg_bit, neg_bit + 1),
                (uint8_t)fld(w, abs_bit, abs_bit + 1), 0 };
    return o;
}

static int alu_srcs(const uint32_t* w, unsigned form, eopnd s[3]) {
    eopnd imm = { K_IMM, 0, 0, 0, (uint32_t)fld(w, 32, 64) };
    eopnd cb = { K_CB, (uint8_t)fld(w, 54, 59), 0, 0, (uint32_t)fld(w, 38, 54) };
    s[0] = reg_at(w, 24, 73, 72);
    switch (form) {
    case 1: s[1] = reg_at(w, 32, 62, 63); s[2] = reg_at(w, 64, 74, 75); return 1;
    case 4: s[1] = imm; s[2] = reg_at(w, 64, 74, 75); return 1;
    case 5: s[1] = cb; s[2] = reg_at(w, 64, 74, 75); return 1;
    case 2: s[1] = reg_at(w, 64, 74, 75); s[2] = imm; return 1;
    case 3: s[1] = reg_at(w, 64, 74, 75); s[2] = cb; return 1;
    default: return 0;
    }
}

static void no_mods(dinstr* d) {
    for (int i = 0; i < 3; i++) d->s[i].neg = d->s[i].abs = 0;
}

static int decode(struct nv_emu* e, const uint32_t* w, uint32_t idx, dinstr* d) {
    unsigned op9 = (unsigned)fld(w, 0, 9), form = (unsigned)fld(w, 9, 12);
    eopnd s[3];
    int alu_ok = alu_srcs(w, form, s);
    memset(d, 0, sizeof(*d));
    d->guard = (uint8_t)fld(w, 12, 15);
    d->guard_inv = (uint8_t)fld(w, 15, 16);
    d->dst = (uint8_t)fld(w, 16, 24);
    d->pred = (uint8_t)fld(w, 87, 90);
    d->pred_inv = (uint8_t)fld(w, 90, 91);
    d->pdst = 7;

#define NEED_ALU() do { if (!alu_ok) goto bad; } while (0)
#define SRCS1(x) do { d->nsrc = 1; d->s[0] = (x); } while (0)
#define SRCS2(x, y) do { d->nsrc = 2; d->s[0] = (x); d->s[1] = (y); } while (0)
#define SRCS3(x, y, z) do { d->nsrc = 3; d->s[0] = (x); d->s[1] = (y); d->s[2] = (z); } while (0)

    switch (op9) {
    case 0x002: NEED_ALU(); d->op = OP_MOV; SRCS1(s[1]); no_mods(d); break;
    case 0x021:
        NEED_ALU(); d->op = OP_FADD;
        if (form == 1) SRCS2(s[0], s[1]);
        else if (form == 2 || form == 3) SRCS2(s[0], s[2]);
        else goto bad;
        d->a = (uint32_t)fld(w, 77, 78);
        d->b = (uint32_t)fld(w, 78, 80);
        break;
    case 0x020: NEED_ALU(); d->op = OP_FMUL; SRCS2(s[0], s[1]); break;
    case 0x023: NEED_ALU(); d->op = OP_FFMA; SRCS3(s[0], s[1], s[2]); break;
    case 0x009: NEED_ALU(); d->op = OP_FMNMX; SRCS2(s[0], s[1]); break;
    case 0x00b:
        NEED_ALU(); d->op = OP_FSETP; SRCS2(s[0], s[1]);
        d->pdst = (uint8_t)fld(w, 81, 84);
        d->a = (uint32_t)fld(w, 76, 80);
        d->b = (uint32_t)fld(w, 74, 76);
        break;
    case 0x024:
        NEED_ALU(); d->op = OP_IMAD; SRCS3(s[0], s[1], s[2]); no_mods(d); break;
    case 0x025:
        /* 64-bit result; the addend is a register pair or a 64-bit cb0 slot */
        NEED_ALU(); d->op = OP_IMADW; SRCS3(s[0], s[1], s[2]); no_mods(d);
        if (form != 1 && form != 3) goto bad;
        d->a = (uint32_t)fld(w, 73, 74);
        break;
    case 0x010:
        NEED_ALU(); d->op = OP_IADD3; SRCS3(s[0], s[1], s[2]);
        for (int i = 0; i < 3; i++) d->s[i].abs = 0;
        break;
    case 0x017:
        NEED_ALU(); d->op = OP_IMNMX; SRCS2(s[0], s[1]); no_mods(d);
        d->a = (uint32_t)fld(w, 73, 74);
        break;
    case 0x00c:
        NEED_ALU(); d->op = OP_ISETP; SRCS2(s[0], s[1]); no_mods(d);
        d->pdst = (uint8_t)fld(w, 81, 84);
        d->a = (uint32_t)fld(w, 76, 79);
        d->b = (uint32_t)fld(w, 74, 76);
        d->c = (uint32_t)fld(w, 73, 74);
        break;
    case 0x012:
        NEED_ALU(); d->op = OP_LOP3; SRCS3(s[0], s[1], s[2]); no_mods(d);
        d->a = (uint32_t)fld(w, 72, 80);
        break;
    case 0x019:
        NEED_ALU(); d->op = OP_SHF; SRCS3(s[0], s[1], s[2]); no_mods(d);
        d->a = (uint32_t)fld(w, 73, 75);                 /* data type */
        d->b = (uint32_t)(fld(w, 75, 76) | fld(w, 76, 77) << 1 |
                          fld(w, 80, 81) << 2);          /* wrap, right, high */
        break;
    case 0x007: NEED_ALU(); d->op = OP_SEL; SRCS2(s[0], s[1]); no_mods(d); break;
    case 0x108:
        NEED_ALU(); d->op = OP_MUFU; SRCS1(s[1]);
        d->a = (uint32_t)fld(w, 74, 80);
        break;
    case 0x105:
        NEED_ALU(); d->op = OP_F2I; SRCS1(s[1]);
        d->a = (uint32_t)fld(w, 72, 73);
        d->b = (uint32_t)fld(w, 78, 80);
        if (fld(w, 75, 77) != 2 || fld(w, 84, 86) != 2) goto bad;
        break;
    case 0x106:
        NEED_ALU(); d->op = OP_I2F; SRCS1(s[1]); no_mods(d);
        d->a = (uint32_t)fld(w, 74, 75);
        d->b = (uint32_t)fld(w, 78, 80);
        if (fld(w, 75, 77) != 2 || fld(w, 84, 86) != 2) goto bad;
        break;
    case 0x107:
        NEED_ALU(); d->op = OP_FRND; SRCS1(s[1]);
        d->b = (uint32_t)fld(w, 78, 80);
        if (fld(w, 75, 77) != 2 || fld(w, 84, 86) != 2) goto bad;
        break;
    case 0x119:
        if (form != 4) goto bad;
        d->op = OP_S2R; d->a = (uint32_t)fld(w, 72, 80);
        break;
    case 0x0b9:
        if (form != 5) goto bad;
        d->op = OP_ULDC;
        d->dst = (uint8_t)fld(w, 16, 22);
        d->off = (int32_t)fld(w, 38, 54);
        d->a = (uint32_t)fld(w, 73, 76);
        if (fld(w, 54, 59) != 0 || (d->a != 4 && d->a != 5)) goto bad;
        break;
    case 0x181:
    case 0x186: {
        if (form != 4) goto bad;
        int st = op9 == 0x186;
        d->op = st ? OP_STG : OP_LDG;
        eopnd addr = { K_REG, (uint8_t)fld(w, 24, 32), 0, 0, 0 };
        eopnd data = { K_REG, (uint8_t)fld(w, 32, 40), 0, 0, 0 };
        if (st) SRCS2(addr, data); else SRCS1(addr);
        d->a = (uint32_t)(st ? fld(w, 64, 70) : fld(w, 32, 38));   /* UR base */
        d->off = (int32_t)sext(fld(w, 40, 64), 24);
        /* 64-bit address, 64-bit UR base, UGPR mode, B32, weak */
        if (!fld(w, 90, 91) || !fld(w, 72, 73) || !fld(w, 91, 92) ||
            fld(w, 73, 76) != 4)
            goto bad;
        break;
    }
    case 0x147: {
        if (form != 4) goto bad;
        d->op = OP_BRA;
        int64_t rel = sext(fld(w, 34, 82), 48);
        if (rel % 4) goto bad;
        d->target = (int32_t)((int64_t)idx + 1 + rel / 4);
        break;
    }
    case 0x14d: if (form != 4) goto bad; d->op = OP_EXIT; break;
    case 0x155:
        if (form != 1 || !fld(w, 84, 85)) goto bad;
        d->op = OP_BCLEAR; d->a = (uint32_t)fld(w, 24, 28);
        break;
    case 0x141:
        if (form != 4) goto bad;
        d->op = OP_BSYNC; d->a = (uint32_t)fld(w, 16, 20);
        break;
    case 0x145: {
        if (form != 4) goto bad;
        d->op = OP_BSSY; d->a = (uint32_t)fld(w, 16, 20);
        int64_t rel = sext(fld(w, 34, 64), 30);
        if (rel % 4) goto bad;
        d->target = (int32_t)((int64_t)idx + 1 + rel / 4);
        break;
    }
    case 0x118: if (form != 4) goto bad; d->op = OP_NOP; break;
    default:
        goto bad;
    }
    return 1;
bad:
    efail(e, "instr %u (0x%04x): cannot decode opcode 0x%03x form %u", idx,
          idx * 16, op9, form);
    return 0;
}

/* ---- disassembly -------------------------------------------------------- */
static int fmt_opnd(char* o, size_t n, const eopnd* s) {
    switch (s->kind) {
    case K_REG: {
        char r[8];
        if (s->reg == 255) snprintf(r, sizeof(r), "RZ");
        else snprintf(r, sizeof(r), "R%u", s->reg);
        return snprintf(o, n, "%s%s%s%s", s->neg ? "-" : "", s->abs ? "|" : "",
                        r, s->abs ? "|" : "");
    }
    case K_IMM: return snprintf(o, n, "#%08x", s->imm);
    case K_CB:  return snprintf(o, n, "c[0x%x][0x%x]", s->reg, s->imm);
    default:    return snprintf(o, n, "?");
    }
}

static const char* pred_name(char* buf, unsigned idx, unsigned inv) {
    if (idx == 7) snprintf(buf, 8, "%sPT", inv ? "!" : "");
    else snprintf(buf, 8, "%sP%u", inv ? "!" : "", idx);
    return buf;
}

void nv_emu_disasm(const struct nv_emu* e, uint32_t idx, char* out, size_t n) {
    static const char* fcmp[16] = { "F", "LT", "EQ", "LE", "GT", "NE", "GE",
        "NUM", "NAN", "LTU", "EQU", "LEU", "GTU", "NEU", "GEU", "T" };
    static const char* icmp[8] = { "F", "LT", "EQ", "LE", "GT", "NE", "GE", "T" };
    static const char* setop[4] = { "AND", "OR", "XOR", "?" };
    static const char* rnd[4] = { "", ".FLOOR", ".CEIL", ".TRUNC" };
    static const char* frm[4] = { "", ".RM", ".RP", ".RZ" };
    static const char* mufu[16] = { "COS", "SIN", "EX2", "LG2", "RCP", "RSQ",
        "RCP64H", "RSQ64H", "SQRT", "TANH", "?", "?", "?", "?", "?", "?" };
    const dinstr* d = &e->prog[idx];
    char g[16] = "", pb[8], pb2[8], s0[32], s1[32], s2[32];
    if (d->guard != 7 || d->guard_inv)
        snprintf(g, sizeof(g), "@%s ", pred_name(pb, d->guard, d->guard_inv));
    fmt_opnd(s0, sizeof(s0), &d->s[0]);
    fmt_opnd(s1, sizeof(s1), &d->s[1]);
    fmt_opnd(s2, sizeof(s2), &d->s[2]);
    char dst[8];
    if (d->dst == 255) snprintf(dst, sizeof(dst), "RZ");
    else snprintf(dst, sizeof(dst), "R%u", d->dst);
    pred_name(pb, d->pred, d->pred_inv);

    switch (d->op) {
    case OP_MOV: snprintf(out, n, "%sMOV %s, %s", g, dst, s0); break;
    case OP_FADD:
        snprintf(out, n, "%sFADD%s%s %s, %s, %s", g, d->a ? ".SAT" : "", frm[d->b & 3],
                 dst, s0, s1);
        break;
    case OP_FMUL: snprintf(out, n, "%sFMUL %s, %s, %s", g, dst, s0, s1); break;
    case OP_FFMA: snprintf(out, n, "%sFFMA %s, %s, %s, %s", g, dst, s0, s1, s2); break;
    case OP_FMNMX: snprintf(out, n, "%sFMNMX %s, %s, %s, %s", g, dst, s0, s1, pb); break;
    case OP_FSETP:
        snprintf(out, n, "%sFSETP.%s.%s %s, PT, %s, %s, %s", g, fcmp[d->a & 15],
                 setop[d->b & 3], pred_name(pb2, d->pdst, 0), s0, s1, pb);
        break;
    case OP_ISETP:
        snprintf(out, n, "%sISETP.%s%s.%s %s, PT, %s, %s, %s", g, icmp[d->a & 7],
                 d->c ? "" : ".U32", setop[d->b & 3],
                 pred_name(pb2, d->pdst, 0), s0, s1, pb);
        break;
    case OP_IMAD: snprintf(out, n, "%sIMAD %s, %s, %s, %s", g, dst, s0, s1, s2); break;
    case OP_IMADW:
        snprintf(out, n, "%sIMAD.WIDE%s %s, %s, %s, %s", g, d->a ? "" : ".U32",
                 dst, s0, s1, s2);
        break;
    case OP_IADD3: snprintf(out, n, "%sIADD3 %s, %s, %s, %s", g, dst, s0, s1, s2); break;
    case OP_IMNMX:
        snprintf(out, n, "%sIMNMX%s %s, %s, %s, %s", g, d->a ? "" : ".U32", dst,
                 s0, s1, pb);
        break;
    case OP_LOP3:
        snprintf(out, n, "%sLOP3.LUT %s, %s, %s, %s, 0x%x, %s", g, dst, s0, s1,
                 s2, d->a, pb);
        break;
    case OP_SHF:
        snprintf(out, n, "%sSHF.%s%s.%s%s %s, %s, %s, %s", g,
                 (d->b & 2) ? "R" : "L", (d->b & 1) ? ".W" : "",
                 d->a == 0 ? "S64" : d->a == 1 ? "U64" : d->a == 2 ? "S32" : "U32",
                 (d->b & 4) ? ".HI" : "", dst, s0, s1, s2);
        break;
    case OP_SEL: snprintf(out, n, "%sSEL %s, %s, %s, %s", g, dst, s0, s1, pb); break;
    case OP_MUFU: snprintf(out, n, "%sMUFU.%s %s, %s", g, mufu[d->a & 15], dst, s0); break;
    case OP_F2I:
        snprintf(out, n, "%sF2I%s%s %s, %s", g, d->a ? "" : ".U32", rnd[d->b & 3],
                 dst, s0);
        break;
    case OP_I2F:
        snprintf(out, n, "%sI2F%s%s %s, %s", g, d->a ? "" : ".U32",
                 d->b == 1 ? ".RM" : d->b == 2 ? ".RP" : d->b == 3 ? ".RZ" : "",
                 dst, s0);
        break;
    case OP_FRND: snprintf(out, n, "%sFRND%s %s, %s", g, rnd[d->b & 3], dst, s0); break;
    case OP_S2R: {
        const char* nm = "?";
        static const char* xyz[3] = { "X", "Y", "Z" };
        char sr[24];
        if (d->a >= 0x21 && d->a <= 0x23) {
            snprintf(sr, sizeof(sr), "SR_TID.%s", xyz[d->a - 0x21]); nm = sr;
        } else if (d->a >= 0x25 && d->a <= 0x27) {
            snprintf(sr, sizeof(sr), "SR_CTAID.%s", xyz[d->a - 0x25]); nm = sr;
        }
        snprintf(out, n, "%sS2R %s, %s", g, dst, nm);
        break;
    }
    case OP_ULDC:
        snprintf(out, n, "%sULDC%s UR%u, c[0x0][0x%x]", g, d->a == 5 ? ".64" : "",
                 d->dst, (unsigned)d->off);
        break;
    case OP_LDG:
    case OP_STG: {
        char mem[40];
        if (d->off)
            snprintf(mem, sizeof(mem), "[R%u.64+0x%x]", d->s[0].reg, (unsigned)d->off);
        else
            snprintf(mem, sizeof(mem), "[R%u.64]", d->s[0].reg);
        if (d->op == OP_LDG) snprintf(out, n, "%sLDG.E %s, %s", g, dst, mem);
        else snprintf(out, n, "%sSTG.E %s, %s", g, mem, s1);
        break;
    }
    case OP_BRA: snprintf(out, n, "%sBRA @0x%04x", g, (unsigned)d->target * 16); break;
    case OP_EXIT: snprintf(out, n, "%sEXIT", g); break;
    case OP_BCLEAR: snprintf(out, n, "%sBMOV.32.CLEAR %s, B%u", g, dst, d->a); break;
    case OP_BSYNC: snprintf(out, n, "%sBSYNC B%u", g, d->a); break;
    case OP_BSSY:
        snprintf(out, n, "%sBSSY B%u, @0x%04x", g, d->a, (unsigned)d->target * 16);
        break;
    default: snprintf(out, n, "%sNOP", g); break;
    }
}

/* ---- create ------------------------------------------------------------- */
struct nv_emu* nv_emu_create(const struct nv_emu_env* env, char* err, size_t errsz) {
    struct nv_emu* e = calloc(1, sizeof(*e));
    e->env = *env;
    e->n = env->ninstr;
    e->err = err;
    e->errsz = errsz;
    e->prog = calloc(e->n ? e->n : 1, sizeof(dinstr));
    for (uint32_t i = 0; i < e->n; i++)
        if (!decode(e, env->code + i * 4, i, &e->prog[i]))
            goto fail;
    for (uint32_t i = 0; i < e->n; i++) {
        const dinstr* d = &e->prog[i];
        if (d->op != OP_BRA && d->op != OP_BSSY)
            continue;
        if (d->target < 0 || (uint32_t)d->target >= e->n) {
            efail(e, "instr %u: branch target out of range", i);
            goto fail;
        }
        if (d->op == OP_BSSY && (e->prog[d->target].op != OP_BSYNC ||
                                 e->prog[d->target].a != d->a)) {
            efail(e, "instr %u: BSSY B%u target is not BSYNC B%u", i, d->a, d->a);
            goto fail;
        }
    }
    if (env->num_gprs < 3 || env->num_gprs > 255) {
        efail(e, "declared register count %u out of range", env->num_gprs);
        goto fail;
    }
    return e;
fail:
    nv_emu_destroy(e);
    return NULL;
}

void nv_emu_destroy(struct nv_emu* e) {
    if (!e) return;
    free(e->prog);
    free(e);
}

uint64_t nv_emu_steps(const struct nv_emu* e) { return e->steps; }

/* ---- execute ------------------------------------------------------------ */
static void where(struct nv_emu* e, lane_t* l, const char* what, unsigned v) {
    char dis[128];
    nv_emu_disasm(e, l->pc, dis, sizeof(dis));
    efail(e, "%s %u at 0x%04x `%s` (cta %u,%u,%u tid %u,%u,%u)", what, v,
          l->pc * 16, dis, e->ctaid[0], e->ctaid[1], e->ctaid[2], l->tid[0],
          l->tid[1], l->tid[2]);
}

static int reg_ok(struct nv_emu* e, lane_t* l, unsigned r) {
    if (r >= e->env.num_gprs - 2) {
        where(e, l, "OOR_REG: register", r);
        return 0;
    }
    return 1;
}

static uint32_t rd(struct nv_emu* e, lane_t* l, unsigned r) {
    if (r == 255) return 0;
    if (!reg_ok(e, l, r)) return 0;
    if (!l->Rinit[r]) {
        where(e, l, "read of unwritten register", r);
        return 0;
    }
    return l->R[r];
}

static void wr(struct nv_emu* e, lane_t* l, unsigned r, uint32_t v) {
    if (r == 255) return;
    if (!reg_ok(e, l, r)) return;
    l->R[r] = v;
    l->Rinit[r] = 1;
}

static int rdp(struct nv_emu* e, lane_t* l, unsigned idx, unsigned inv) {
    if (idx == 7) return !inv;
    if (!l->Pinit[idx]) {
        where(e, l, "read of unwritten predicate", idx);
        return 0;
    }
    return l->P[idx] ^ (int)inv;
}

static uint32_t cb0_rd(struct nv_emu* e, lane_t* l, uint32_t off) {
    uint32_t v = 0;
    if (off + 4 > e->env.cb0_size) {
        where(e, l, "constant bank read past end, offset", off);
        return 0;
    }
    memcpy(&v, e->env.cb0 + off, 4);
    return v;
}

/* integer-view operand (neg = two's complement) */
static uint32_t ival(struct nv_emu* e, lane_t* l, const eopnd* s) {
    uint32_t v;
    switch (s->kind) {
    case K_REG: v = rd(e, l, s->reg); break;
    case K_IMM: return s->imm;
    case K_CB:  return cb0_rd(e, l, s->imm);
    default:    return 0;
    }
    return s->neg ? 0u - v : v;
}

/* float-view operand (abs, then neg, on the sign bit) */
static float fval(struct nv_emu* e, lane_t* l, const eopnd* s) {
    uint32_t v;
    switch (s->kind) {
    case K_REG: v = rd(e, l, s->reg); break;
    case K_IMM: return ref_u2f(s->imm);
    case K_CB:  return ref_u2f(cb0_rd(e, l, s->imm));
    default:    return 0.0f;
    }
    if (s->abs) v &= 0x7fffffffu;
    if (s->neg) v ^= 0x80000000u;
    return ref_u2f(v);
}

static uint8_t* mem_at(struct nv_emu* e, lane_t* l, uint64_t va) {
    for (unsigned i = 0; i < e->env.nmem; i++) {
        const struct nv_emu_mem* m = &e->env.mem[i];
        if (va >= m->va && va + 4 <= m->va + m->size)
            return m->p + (va - m->va);
    }
    where(e, l, "global access outside any buffer, low word", (unsigned)va);
    return NULL;
}

static int fcmp_eval(unsigned op, float a, float b) {
    int un = (a != a) || (b != b);
    switch (op) {
    case 0: return 0;
    case 1: return !un && a < b;
    case 2: return !un && a == b;
    case 3: return !un && a <= b;
    case 4: return !un && a > b;
    case 5: return !un && a != b;
    case 6: return !un && a >= b;
    case 7: return !un;
    case 8: return un;
    case 9: return un || a < b;
    case 10: return un || a == b;
    case 11: return un || a <= b;
    case 12: return un || a > b;
    case 13: return un || a != b;
    case 14: return un || a >= b;
    default: return 1;
    }
}

static int icmp_eval(unsigned op, int is_signed, uint32_t a, uint32_t b) {
    int lt = is_signed ? (int32_t)a < (int32_t)b : a < b;
    int eq = a == b;
    switch (op) {
    case 1: return lt;
    case 2: return eq;
    case 3: return lt || eq;
    case 4: return !lt && !eq;
    case 5: return !eq;
    case 6: return !lt;
    case 7: return 1;
    default: return 0;
    }
}

static int setop_eval(unsigned op, int v, int accum) {
    return op == 0 ? (v && accum) : op == 1 ? (v || accum) : (v ^ accum);
}

static void exec(struct nv_emu* e, lane_t* l, const dinstr* d) {
    const eopnd* s = d->s;
    switch (d->op) {
    case OP_MOV: wr(e, l, d->dst, ival(e, l, &s[0])); break;
    case OP_FADD: {
        float r = ref_fadd(fval(e, l, &s[0]), fval(e, l, &s[1]), (int)d->b);
        if (d->a) r = ref_fsat(r);
        wr(e, l, d->dst, ref_f2u_bits(r));
        break;
    }
    case OP_FMUL:
        wr(e, l, d->dst, ref_f2u_bits(fval(e, l, &s[0]) * fval(e, l, &s[1])));
        break;
    case OP_FFMA:
        wr(e, l, d->dst, ref_f2u_bits(fmaf(fval(e, l, &s[0]), fval(e, l, &s[1]),
                                           fval(e, l, &s[2]))));
        break;
    case OP_FMNMX: {
        float a = fval(e, l, &s[0]), b = fval(e, l, &s[1]);
        int mn = rdp(e, l, d->pred, d->pred_inv);
        wr(e, l, d->dst, ref_f2u_bits(mn ? ref_fmin(a, b) : ref_fmax(a, b)));
        break;
    }
    case OP_FSETP: {
        int v = fcmp_eval(d->a, fval(e, l, &s[0]), fval(e, l, &s[1]));
        v = setop_eval(d->b, v, rdp(e, l, d->pred, d->pred_inv));
        if (d->pdst != 7) { l->P[d->pdst] = (uint8_t)v; l->Pinit[d->pdst] = 1; }
        break;
    }
    case OP_ISETP: {
        int v = icmp_eval(d->a, (int)d->c, ival(e, l, &s[0]), ival(e, l, &s[1]));
        v = setop_eval(d->b, v, rdp(e, l, d->pred, d->pred_inv));
        if (d->pdst != 7) { l->P[d->pdst] = (uint8_t)v; l->Pinit[d->pdst] = 1; }
        break;
    }
    case OP_IMAD:
        wr(e, l, d->dst, ival(e, l, &s[0]) * ival(e, l, &s[1]) + ival(e, l, &s[2]));
        break;
    case OP_IMADW: {
        uint32_t a = ival(e, l, &s[0]), b = ival(e, l, &s[1]);
        uint64_t c;
        if (s[2].kind == K_CB) {
            c = cb0_rd(e, l, s[2].imm) | (uint64_t)cb0_rd(e, l, s[2].imm + 4) << 32;
        } else if (s[2].reg == 255) {
            c = 0;
        } else {
            if (s[2].reg & 1) where(e, l, "MISALIGNED_REG: 64-bit addend", s[2].reg);
            c = rd(e, l, s[2].reg) | (uint64_t)rd(e, l, s[2].reg + 1u) << 32;
        }
        uint64_t r = d->a ? (uint64_t)((int64_t)(int32_t)a * (int64_t)(int32_t)b)
                          : (uint64_t)a * b;
        r += c;
        if (d->dst & 1) where(e, l, "MISALIGNED_REG: 64-bit destination", d->dst);
        wr(e, l, d->dst, (uint32_t)r);
        wr(e, l, d->dst + 1u, (uint32_t)(r >> 32));
        break;
    }
    case OP_IADD3:
        wr(e, l, d->dst, ival(e, l, &s[0]) + ival(e, l, &s[1]) + ival(e, l, &s[2]));
        break;
    case OP_IMNMX: {
        uint32_t a = ival(e, l, &s[0]), b = ival(e, l, &s[1]);
        int mn = rdp(e, l, d->pred, d->pred_inv);
        int lt = d->a ? (int32_t)a < (int32_t)b : a < b;
        wr(e, l, d->dst, (mn ? lt : !lt) ? a : b);
        break;
    }
    case OP_LOP3: {
        uint32_t a = ival(e, l, &s[0]), b = ival(e, l, &s[1]), c = ival(e, l, &s[2]);
        uint32_t r = 0;
        for (unsigned i = 0; i < 8; i++) {
            if (!(d->a & (1u << i))) continue;
            r |= ((i & 4) ? a : ~a) & ((i & 2) ? b : ~b) & ((i & 1) ? c : ~c);
        }
        wr(e, l, d->dst, r);
        break;
    }
    case OP_SHF: {
        uint32_t lo = ival(e, l, &s[0]), sh = ival(e, l, &s[1]), hi = ival(e, l, &s[2]);
        int wrap = d->b & 1, right = d->b & 2, high = d->b & 4;
        if (d->a < 2) {
            where(e, l, "64-bit SHF data type", d->a);
            break;
        }
        int is_signed = d->a == 2;
        if (wrap) sh &= 31; else if (sh > 32) sh = 32;
        uint64_t v = ((uint64_t)hi << 32) | lo, r;
        if (!right) r = v << sh;
        else if (is_signed) r = (uint64_t)((int64_t)v >> sh);
        else r = v >> sh;
        wr(e, l, d->dst, high ? (uint32_t)(r >> 32) : (uint32_t)r);
        break;
    }
    case OP_SEL:
        if (rdp(e, l, d->pred, d->pred_inv))
            wr(e, l, d->dst, ival(e, l, &s[0]));
        else
            wr(e, l, d->dst, ival(e, l, &s[1]));
        break;
    case OP_MUFU: {
        float x = fval(e, l, &s[0]), r;
        switch (d->a) {
        case 0: r = ref_cos_turns(x); break;
        case 1: r = ref_sin_turns(x); break;
        case 2: r = exp2f(x); break;
        case 3: r = log2f(x); break;
        case 4: r = ref_frcp(x); break;
        case 5: r = ref_frsq(x); break;
        case 8: r = sqrtf(x); break;
        default: where(e, l, "unsupported MUFU op", d->a); r = 0; break;
        }
        wr(e, l, d->dst, ref_f2u_bits(r));
        break;
    }
    case OP_F2I: {
        float x = ref_frnd(fval(e, l, &s[0]), (int)d->b);
        wr(e, l, d->dst, d->a ? (uint32_t)ref_f2i32(x) : ref_f2u32(x));
        break;
    }
    case OP_I2F: {
        uint32_t v = ival(e, l, &s[0]);
        if (d->b) where(e, l, "I2F rounding mode", d->b);
        wr(e, l, d->dst, ref_f2u_bits(d->a ? (float)(int32_t)v : (float)v));
        break;
    }
    case OP_FRND:
        wr(e, l, d->dst, ref_f2u_bits(ref_frnd(fval(e, l, &s[0]), (int)d->b)));
        break;
    case OP_S2R: {
        uint32_t v = 0;
        if (d->a >= 0x21 && d->a <= 0x23) v = l->tid[d->a - 0x21];
        else if (d->a >= 0x25 && d->a <= 0x27) v = e->ctaid[d->a - 0x25];
        else where(e, l, "unsupported special register", d->a);
        wr(e, l, d->dst, v);
        break;
    }
    case OP_ULDC: {
        uint64_t v = cb0_rd(e, l, (uint32_t)d->off);
        if (d->a == 5) {
            if (d->dst & 1) where(e, l, "odd uniform register pair", d->dst);
            v |= (uint64_t)cb0_rd(e, l, (uint32_t)d->off + 4) << 32;
            e->UR[d->dst] = v & 0xffffffffu;
            e->UR[d->dst + 1] = v >> 32;
            e->URinit[d->dst] = e->URinit[d->dst + 1] = 1;
        } else {
            e->UR[d->dst] = v;
            e->URinit[d->dst] = 1;
        }
        break;
    }
    case OP_LDG:
    case OP_STG: {
        unsigned ar = s[0].reg;
        if (ar & 1) where(e, l, "MISALIGNED_REG: address pair", ar);
        uint64_t va = (uint64_t)rd(e, l, ar) | (uint64_t)rd(e, l, ar + 1) << 32;
        if (d->a != 63) {
            if ((d->a & 1) || !e->URinit[d->a] || !e->URinit[d->a + 1])
                where(e, l, "bad uniform base register", d->a);
            /* GA10B raises INVALID_ADDR_SPACE (warp_esr 0x10) when this
             * operand carries a buffer address; nvcc only ever loads it from
             * c[0x0][0x118], which the launch leaves zero. */
            else if (e->UR[d->a] | e->UR[d->a + 1])
                where(e, l, "INVALID_ADDR_SPACE: non-zero uniform base UR", d->a);
        }
        va += (uint64_t)(int64_t)d->off;
        if (e->failed) break;
        uint8_t* p = mem_at(e, l, va);
        if (!p) break;
        if (d->op == OP_LDG) {
            uint32_t v;
            memcpy(&v, p, 4);
            wr(e, l, d->dst, v);
        } else {
            uint32_t v = ival(e, l, &s[1]);
            memcpy(p, &v, 4);
        }
        break;
    }
    default:
        break;
    }
}

static int run_warp(struct nv_emu* e, unsigned first, unsigned nthreads) {
    const uint32_t* blk = e->env.block;
    uint32_t exited = 0;
    for (unsigned i = 0; i < LANES; i++) {
        lane_t* l = &e->L[i];
        unsigned t = first + i;
        memset(l->Rinit, 0, sizeof(l->Rinit));
        memset(l->Pinit, 0, sizeof(l->Pinit));
        l->pc = 0;
        if (t >= nthreads) {
            l->state = ST_ABSENT;
            exited |= 1u << i;
            continue;
        }
        l->state = ST_RUN;
        l->tid[0] = t % blk[0];
        l->tid[1] = (t / blk[0]) % blk[1];
        l->tid[2] = t / (blk[0] * blk[1]);
    }
    memset(e->Bpoison, 1, sizeof(e->Bpoison));
    memset(e->URinit, 0, sizeof(e->URinit));

    uint64_t budget = 400u * 1000u * 1000u, start = e->steps;
    for (;;) {
        uint32_t minpc = UINT32_MAX;
        for (unsigned i = 0; i < LANES; i++)
            if (e->L[i].state == ST_RUN && e->L[i].pc < minpc)
                minpc = e->L[i].pc;

        if (minpc == UINT32_MAX) {
            /* nothing runnable: release every BSYNC whose armed lanes are all
             * parked on it (exited lanes never hold one up) */
            int released = 0, waiting = 0;
            for (unsigned i = 0; i < LANES; i++) {
                if (e->L[i].state != ST_WAIT)
                    continue;
                waiting = 1;
                uint32_t pc = e->L[i].pc, here = 0;
                for (unsigned k = 0; k < LANES; k++)
                    if (e->L[k].state == ST_WAIT && e->L[k].pc == pc)
                        here |= 1u << k;
                uint32_t need = e->B[e->prog[pc].a] & ~exited;
                if (need & ~here)
                    continue;
                for (unsigned k = 0; k < LANES; k++)
                    if (here & (1u << k)) {
                        e->L[k].state = ST_RUN;
                        e->L[k].pc = pc + 1;
                    }
                released = 1;
            }
            if (released)
                continue;
            if (waiting) {
                for (unsigned i = 0; i < LANES; i++)
                    if (e->L[i].state == ST_WAIT) {
                        where(e, &e->L[i], "deadlock: lane parked at BSYNC B",
                              e->prog[e->L[i].pc].a);
                        break;
                    }
                return -1;
            }
            return 0;
        }
        if (minpc >= e->n) {
            efail(e, "execution ran past the end of the kernel");
            return -1;
        }

        const dinstr* d = &e->prog[minpc];
        uint32_t group = 0;
        for (unsigned i = 0; i < LANES; i++)
            if (e->L[i].state == ST_RUN && e->L[i].pc == minpc)
                group |= 1u << i;

        if (d->op == OP_BSSY) {
            if (e->Bpoison[d->a])
                where(e, &e->L[__builtin_ctz(group)], "BSSY on uncleared barrier B", d->a);
            e->B[d->a] |= group;
        } else if (d->op == OP_BCLEAR) {
            for (unsigned i = 0; i < LANES; i++)
                if (e->L[i].state == ST_WAIT && e->prog[e->L[i].pc].a == d->a)
                    where(e, &e->L[i], "barrier cleared while a lane waits on B", d->a);
            e->B[d->a] = 0;
            e->Bpoison[d->a] = 0;
        }

        for (unsigned i = 0; i < LANES && !e->failed; i++) {
            if (!(group & (1u << i)))
                continue;
            lane_t* l = &e->L[i];
            e->steps++;
            int on = rdp(e, l, d->guard, d->guard_inv);
            switch (d->op) {
            case OP_BRA:
                if (on && rdp(e, l, d->pred, d->pred_inv)) l->pc = (uint32_t)d->target;
                else l->pc++;
                break;
            case OP_EXIT:
                if (on) { l->state = ST_EXIT; exited |= 1u << i; }
                else l->pc++;
                break;
            case OP_BSYNC:
                if (e->Bpoison[d->a] || !(e->B[d->a] & (1u << i)))
                    where(e, l, "lane reached BSYNC without being armed on B", d->a);
                l->state = ST_WAIT;
                break;
            case OP_BSSY:
            case OP_BCLEAR:
            case OP_NOP:
                l->pc++;
                break;
            default:
                if (on) exec(e, l, d);
                l->pc++;
                break;
            }
        }
        if (e->failed)
            return -1;
        if (e->steps - start > budget) {
            efail(e, "step budget exhausted (runaway loop?) at 0x%04x", minpc * 16);
            return -1;
        }
    }
}

int nv_emu_run_cta(struct nv_emu* e, const uint32_t ctaid[3], char* err,
                   size_t errsz) {
    e->err = err;
    e->errsz = errsz;
    e->failed = 0;
    memcpy(e->ctaid, ctaid, sizeof(e->ctaid));
    unsigned nthreads = e->env.block[0] * e->env.block[1] * e->env.block[2];
    for (unsigned first = 0; first < nthreads; first += LANES)
        if (run_warp(e, first, nthreads) != 0)
            return -1;
    return 0;
}
