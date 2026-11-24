/*
 * emit_probe.c - encode a curated instruction probe with nv_sass and write the
 * raw 128-bit words to a file, for the nvdisasm round-trip test (roundtrip.py).
 * Each instruction is functionally verified by disassembling it with NVIDIA's
 * own nvdisasm and checking the mnemonic/operands.
 *
 * Build: cc -std=c99 -I.. emit_probe.c ../nv_sass.c -o emit_probe
 * Run:   ./emit_probe probe.bin
 * SPDX-License-Identifier: MIT
 */
#include "nv_sass.h"
#include <stdio.h>

static nv_ctrl ctl(int delay) {
    nv_ctrl c = nv_ctrl_none();
    c.delay = (uint8_t)delay;
    return c;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: emit_probe out.bin\n"); return 2; }
    uint32_t w[4];
    FILE* f = fopen(argv[1], "wb");
    if (!f) { perror("fopen"); return 2; }

#define EMIT(call) do { call; fwrite(w, 4, 4, f); } while (0)

    /* integer / address setup */
    EMIT(nv_s2r(w, 6, NV_SR_CTAID_X, ctl(4)));
    EMIT(nv_s2r(w, 3, NV_SR_TID_X, ctl(4)));
    EMIT(nv_imad(w, 6, nv_gpr(6), nv_cb(0, 0), nv_gpr(3), 1, ctl(5)));
    EMIT(nv_mov(w, 1, nv_cb(0, 0x28), ctl(4)));
    EMIT(nv_mov(w, 2, nv_imm(0x12345678), ctl(4)));
    EMIT(nv_mov(w, 2, nv_gpr(3), ctl(4)));
    EMIT(nv_umov(w, 4, nv_imm(1), ctl(4)));

    /* float ALU */
    EMIT(nv_fadd(w, 9, nv_gpr(4), nv_gpr(3), 0, 0, NV_RND_NEAREST, ctl(4)));
    EMIT(nv_fmul(w, 9, nv_gpr(4), nv_gpr(3), 0, 0, NV_RND_NEAREST, ctl(4)));
    EMIT(nv_ffma(w, 9, nv_gpr(4), nv_gpr(3), nv_gpr(2), 0, 0, NV_RND_NEAREST, ctl(4)));
    EMIT(nv_fadd(w, 9, nv_gpr(4), nv_imm(0x3f800000), 0, 0, NV_RND_NEAREST, ctl(4)));
    EMIT(nv_fmnmx(w, 9, nv_gpr(4), nv_gpr(3), nv_pt(), 0, ctl(4)));

    /* integer ALU */
    EMIT(nv_iadd3(w, 9, nv_gpr(4), nv_gpr(3), nv_gpr(2), ctl(4)));
    EMIT(nv_imad(w, 9, nv_gpr(4), nv_gpr(3), nv_gpr(2), 1, ctl(4)));
    EMIT(nv_imad_wide(w, 4, nv_gpr(6), nv_gpr(7), nv_cb(0, 0x168), 1, ctl(4)));
    EMIT(nv_imnmx(w, 9, nv_gpr(4), nv_gpr(3), nv_pt(), 1, ctl(4)));
    EMIT(nv_lop3(w, 9, nv_gpr(4), nv_gpr(3), nv_gpr(2), 0xca, nv_pt(), ctl(4)));
    EMIT(nv_shf(w, 9, nv_gpr(4), nv_gpr(3), nv_imm(2), 1, 0, 0, 0, 0, ctl(4)));
    EMIT(nv_sel(w, 9, nv_gpr(4), nv_gpr(3), nv_p(0), ctl(4)));

    /* compares */
    EMIT(nv_isetp(w, 0, nv_gpr(6), nv_cb(0, 0x178), NV_ICMP_GE, NV_SET_AND,
                  nv_pt(), 1, 0, ctl(4)));
    EMIT(nv_isetp(w, 0, nv_zero(), nv_ugpr(2), NV_ICMP_NE, NV_SET_AND,
                  nv_pt(), 0, 0, ctl(4)));
    EMIT(nv_fsetp(w, 0, nv_gpr(4), nv_gpr(3), NV_FCMP_OGT, NV_SET_AND,
                  nv_pt(), 0, ctl(4)));

    /* conversions */
    EMIT(nv_f2f(w, 9, nv_gpr(4), 32, 64, NV_RND_NEAREST, 0, ctl(4)));
    EMIT(nv_f2i(w, 9, nv_gpr(4), 1, 32, 32, NV_RND_ZERO, 0, ctl(4)));
    EMIT(nv_i2f(w, 9, nv_gpr(4), 1, 32, 32, NV_RND_NEAREST, ctl(4)));
    EMIT(nv_mufu(w, 9, nv_gpr(4), NV_MUFU_RCP, ctl(4)));

    /* memory (NAK model: full VA in addr GPR, uniform base = URZ) */
    EMIT(nv_ldg(w, 4, nv_gpr64(4), 1, nv_ugpr64(4), 0, NV_MT_B32, NV_ORD_WEAK,
                NV_EV_NORMAL, 0, ctl(4)));
    EMIT(nv_stg(w, nv_gpr64(6), 1, nv_ugpr64(4), nv_gpr(9), 0, NV_MT_B32,
                NV_ORD_WEAK, NV_EV_NORMAL, ctl(4)));
    EMIT(nv_lds(w, 4, nv_gpr(2), 0, NV_MT_B32, ctl(4)));
    EMIT(nv_sts(w, nv_gpr(2), nv_gpr(9), 0, NV_MT_B32, ctl(4)));
    EMIT(nv_ldc(w, 4, nv_zero(), 0, 0x160, NV_MT_B32, ctl(4)));
    EMIT(nv_uldc(w, 4, 0, 0x118, NV_MT_B64, 1, ctl(4)));

    /* special regs / uniform */
    EMIT(nv_cs2r(w, 4, NV_SR_CLOCK_LO, 0, ctl(4)));
    EMIT(nv_r2ur(w, 4, nv_gpr(3), ctl(4)));

    /* control flow */
    EMIT(nv_bar(w, ctl(4)));
    EMIT(nv_membar(w, 2 /*GPU*/, ctl(4)));
    EMIT(nv_bmov(w, 0, nv_gpr(3), 1, 0, ctl(4)));
    EMIT(nv_bsync(w, 0, nv_pt(), ctl(4)));
    EMIT(nv_warpsync(w, 0xffffffff, ctl(4)));
    EMIT(nv_bra(w, 4, nv_pt(), ctl(4)));
    EMIT(nv_exit(w, ctl(4)));
    EMIT(nv_nop(w, ctl(4)));

#undef EMIT
    fclose(f);
    return 0;
}