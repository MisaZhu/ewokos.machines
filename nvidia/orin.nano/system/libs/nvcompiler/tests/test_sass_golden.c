/*
 * test_sass_golden.c - bit-exact verification of nv_sass against nvcc sm_87.
 *
 * Golden 128-bit instructions are taken from `nvcc -arch=sm_87 -O3 -cubin` of
 * a vector-add kernel, disassembled with nvdisasm (see kernels/golden/vadd.txt).
 * For each instruction we re-encode the SAME operands with nv_sass, copy nvcc's
 * scheduling control word (bits 105..126) so only the semantic encoding is under
 * test, and compare all 128 bits.  Bits nvcc leaves as don't-care (e.g. the
 * unused GPR dst of ISETP, which NAK sets to RZ=255 but nvcc to 0) are masked.
 *
 * Host build:  cc -std=c99 -I.. test_sass_golden.c ../nv_sass.c -o test_sass
 * SPDX-License-Identifier: MIT
 */
#include "nv_sass.h"
#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;

/* rebuild nv_ctrl from a golden 128-bit instruction */
static nv_ctrl ctrl_from(uint64_t hi, uint64_t lo) {
    uint32_t w[4] = { (uint32_t)lo, (uint32_t)(lo >> 32),
                      (uint32_t)hi, (uint32_t)(hi >> 32) };
    nv_ctrl c;
    c.delay   = (uint8_t)nv_get_field(w, 105, 109);
    c.yld     = (uint8_t)nv_get_field(w, 109, 110);
    c.wr_bar  = (uint8_t)nv_get_field(w, 110, 113);
    c.rd_bar  = (uint8_t)nv_get_field(w, 113, 116);
    c.wt_mask = (uint8_t)nv_get_field(w, 116, 122);
    c.reuse   = (uint8_t)nv_get_field(w, 122, 126);
    return c;
}

static void words_from(uint64_t hi, uint64_t lo, uint32_t w[4]) {
    w[0] = (uint32_t)lo; w[1] = (uint32_t)(lo >> 32);
    w[2] = (uint32_t)hi; w[3] = (uint32_t)(hi >> 32);
}

/* compare encoded out[4] to golden, ignoring don't-care bits in dc[4] */
static void check(const char* name, const uint32_t out[4],
                  uint64_t ghi, uint64_t glo, const uint32_t dc[4]) {
    uint32_t gw[4];
    words_from(ghi, glo, gw);
    int ok = 1;
    for (int i = 0; i < 4; i++) {
        uint32_t diff = (out[i] ^ gw[i]) & ~dc[i];
        if (diff) {
            ok = 0;
            printf("  FAIL %-22s word%d got=%08x want=%08x diff=%08x\n",
                   name, i, out[i], gw[i], diff);
        }
    }
    if (ok) { g_pass++; printf("  PASS %s\n", name); }
    else g_fail++;
}

#define DC_NONE { 0, 0, 0, 0 }
/* ISETP unused GPR dst [16:24] is don't-care */
#define DC_ISETP { 0x00ff0000u, 0, 0, 0 }

int main(void) {
    uint32_t w[4];
    uint32_t dc_none[4] = DC_NONE;
    uint32_t dc_isetp[4] = DC_ISETP;

    printf("nv_sass sm_87 golden verification (vs nvcc 13.5 vadd):\n");

    /* ISETP.NE.U32.AND P0, PT, RZ, UR2, PT  (uniform src form) */
    {
        uint64_t hi = 0x000fda000bf05070ull, lo = 0x00000002ff007c0cull;
        nv_isetp(w, 0, nv_zero(), nv_ugpr(2), NV_ICMP_NE, NV_SET_AND,
                 nv_pt(), 0 /*U32*/, 0, ctrl_from(hi, lo));
        check("ISETP.NE.U32", w, hi, lo, dc_isetp);
    }
    /* @P0 BRA  (rel offset patched separately; here compare opcode+pred) */
    {
        uint64_t hi = 0x000fea0003800000ull, lo = 0x0000012000000947ull;
        /* golden rel = 72 dwords; @P0 carried in the guard field */
        nv_bra(w, 72, nv_pt(), ctrl_from(hi, lo));
        nv_set_guard(w, nv_p(0));
        check("BRA", w, hi, lo, dc_none);
    }
    /* MOV R1, c[0x0][0x28] */
    {
        uint64_t hi = 0x000fe40000000f00ull, lo = 0x00000a0000017a02ull;
        nv_mov(w, 1, nv_cb(0, 0x28), ctrl_from(hi, lo));
        check("MOV cb", w, hi, lo, dc_none);
    }
    /* S2R R6, SR_CTAID.X */
    {
        uint64_t hi = 0x000e280000002500ull, lo = 0x0000000000067919ull;
        nv_s2r(w, 6, NV_SR_CTAID_X, ctrl_from(hi, lo));
        check("S2R CTAID.X", w, hi, lo, dc_none);
    }
    /* S2R R3, SR_TID.X */
    {
        uint64_t hi = 0x000e240000002100ull, lo = 0x0000000000037919ull;
        nv_s2r(w, 3, NV_SR_TID_X, ctrl_from(hi, lo));
        check("S2R TID.X", w, hi, lo, dc_none);
    }
    /* IMAD R6, R6, c[0x0][0x0], R3 */
    {
        uint64_t hi = 0x001fca00078e0203ull, lo = 0x0000000006067a24ull;
        nv_imad(w, 6, nv_gpr(6), nv_cb(0, 0), nv_gpr(3), 1, ctrl_from(hi, lo));
        check("IMAD", w, hi, lo, dc_none);
    }
    /* ISETP.GE.AND P0, PT, R6, c[0x0][0x178], PT */
    {
        uint64_t hi = 0x000fda0003f06270ull, lo = 0x00005e0006007a0cull;
        nv_isetp(w, 0, nv_gpr(6), nv_cb(0, 0x178), NV_ICMP_GE, NV_SET_AND,
                 nv_pt(), 1 /*I32*/, 0, ctrl_from(hi, lo));
        check("ISETP.GE", w, hi, lo, dc_isetp);
    }
    /* @P0 EXIT */
    {
        uint64_t hi = 0x000fea0003800000ull, lo = 0x000000000000094dull;
        nv_exit(w, ctrl_from(hi, lo));
        /* EXIT guard is @P0 in golden; nv_exit hardcodes PT guard.  Apply. */
        nv_set_guard(w, nv_p(0));
        check("EXIT @P0", w, hi, lo, dc_none);
    }
    /* ULDC.64 UR4, c[0x0][0x118] */
    {
        uint64_t hi = 0x000fd20000000a00ull, lo = 0x0000460000047ab9ull;
        nv_uldc(w, 4, 0, 0x118, NV_MT_B64, 1, ctrl_from(hi, lo));
        check("ULDC.64", w, hi, lo, dc_none);
    }
    /* IMAD.WIDE R4, R6, R7, c[0x0][0x168] */
    {
        uint64_t hi = 0x000fc800078e0207ull, lo = 0x00005a0006047625ull;
        nv_imad_wide(w, 4, nv_gpr(6), nv_gpr(7), nv_cb(0, 0x168), 1,
                     ctrl_from(hi, lo));
        check("IMAD.WIDE", w, hi, lo, dc_none);
    }
    /* LDG.E R4, [R4.64] */
    {
        uint64_t hi = 0x000ea8000c1e1900ull, lo = 0x0000000404047981ull;
        nv_ldg(w, 4, nv_gpr(4), 1, nv_ugpr64(4), 0, NV_MT_B32, NV_ORD_WEAK,
               NV_EV_NORMAL, 0, ctrl_from(hi, lo));
        check("LDG.E", w, hi, lo, dc_none);
    }
    /* FADD R9, R4, R3 */
    {
        uint64_t hi = 0x004fca0000000000ull, lo = 0x0000000304097221ull;
        nv_fadd(w, 9, nv_gpr(4), nv_gpr(3), 0, 0, NV_RND_NEAREST,
                ctrl_from(hi, lo));
        check("FADD", w, hi, lo, dc_none);
    }
    /* STG.E [R6.64], R9 */
    {
        uint64_t hi = 0x000fe2000c101904ull, lo = 0x0000000906007986ull;
        nv_stg(w, nv_gpr(6), 1, nv_ugpr64(4), nv_gpr(9), 0, NV_MT_B32,
               NV_ORD_WEAK, NV_EV_NORMAL, ctrl_from(hi, lo));
        check("STG.E", w, hi, lo, dc_none);
    }
    /* EXIT (unconditional) */
    {
        uint64_t hi = 0x000fea0003800000ull, lo = 0x000000000000794dull;
        nv_exit(w, ctrl_from(hi, lo));
        check("EXIT", w, hi, lo, dc_none);
    }
    /* NOP */
    {
        uint64_t hi = 0x000fc00000000000ull, lo = 0x0000000000007918ull;
        nv_nop(w, ctrl_from(hi, lo));
        check("NOP", w, hi, lo, dc_none);
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}