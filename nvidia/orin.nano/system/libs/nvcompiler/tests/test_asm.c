/*
 * test_asm.c - verify the label-resolving assembler's branch patching.
 * Builds a small loop and a forward branch, checks the encoded rel offsets
 * (dword units, target - ip - 4) match hand-computed values.  Host-only.
 * Build: cc -std=c99 -I.. test_asm.c ../nv_sass.c -o test_asm
 * SPDX-License-Identifier: MIT
 */
#include "nv_sass.h"
#include <stdio.h>

static int g_pass, g_fail;

/* sign-extend an n-bit field to int64 */
static int64_t sext(uint64_t v, unsigned n) {
    uint64_t sign = 1ull << (n - 1);
    return (int64_t)((v ^ sign) - sign);
}

static void expect(const char* name, int64_t got, int64_t want) {
    if (got == want) { g_pass++; printf("  PASS %s = %lld\n", name, (long long)got); }
    else { g_fail++; printf("  FAIL %s got=%lld want=%lld\n", name,
                            (long long)got, (long long)want); }
}

int main(void) {
    nv_asm a;
    nv_asm_init(&a);
    nv_ctrl c = nv_ctrl_none();
    c.delay = 4;
    uint32_t w[4];

    printf("nv_sass assembler branch patching:\n");

    /* loop:
     *   0: S2R R0, TID.X        (instr 0, ip=0 dwords)
     *   1: IADD3 R0,R0,R0,RZ    (instr 1)
     *   2: BRA back to instr 1  (instr 2, ip=8; target instr1 ip=4)
     *   3: EXIT                 (instr 3)
     */
    int loop = nv_asm_label(&a);
    nv_s2r(w, 0, NV_SR_TID_X, c); nv_asm_emit(&a, w);   /* instr 0 */
    nv_asm_bind(&a, loop);                              /* loop at instr 1 */
    nv_iadd3(w, 0, nv_gpr(0), nv_gpr(0), nv_zero(), c); nv_asm_emit(&a, w); /* 1 */
    nv_asm_bra(&a, loop, nv_pt(), c);                   /* instr 2 */
    nv_exit(w, c); nv_asm_emit(&a, w);                  /* instr 3 */
    expect("finish", nv_asm_finish(&a), 0);
    /* backward bra at instr2 (ip=8 dwords) -> loop instr1 (ip=4): 4-8-4 = -8 */
    expect("loop rel", sext(nv_get_field(a.w[2], 34, 82), 48), -8);

    /* forward branch:
     *   0: BRA to end (instr 0, ip=0; target instr3 ip=12): 12-0-4 = 8
     *   1: NOP
     *   2: NOP
     *   3: EXIT (end)
     */
    nv_asm b;
    nv_asm_init(&b);
    int end = nv_asm_label(&b);
    nv_asm_bra(&b, end, nv_pt(), c);                    /* instr 0 */
    nv_nop(w, c); nv_asm_emit(&b, w);                   /* 1 */
    nv_nop(w, c); nv_asm_emit(&b, w);                   /* 2 */
    nv_asm_bind(&b, end);
    nv_exit(w, c); nv_asm_emit(&b, w);                  /* 3 */
    expect("finish fwd", nv_asm_finish(&b), 0);
    expect("fwd rel", sext(nv_get_field(b.w[0], 34, 82), 48), 8);

    /* unbound label must fail */
    nv_asm d;
    nv_asm_init(&d);
    int missing = nv_asm_label(&d);
    nv_asm_bra(&d, missing, nv_pt(), c);
    expect("unbound fails", nv_asm_finish(&d), -1);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}