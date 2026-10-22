/* Separate from PSCI: Qualcomm interrupted calls return a continuation in x6.
 * q8b_scm_smc(uint64_t regs[8]); SMCCC preserves x19..x30. */
    .text
    .global q8b_scm_smc
    .type q8b_scm_smc, %function
q8b_scm_smc:
    stp x19, x30, [sp, #-16]!
    mov x19, x0
    ldp x0, x1, [x19]
    ldp x2, x3, [x19, #16]
    ldp x4, x5, [x19, #32]
    ldp x6, x7, [x19, #48]
    smc #0
    stp x0, x1, [x19]
    stp x2, x3, [x19, #16]
    stp x4, x5, [x19, #32]
    stp x6, x7, [x19, #48]
    ldp x19, x30, [sp], #16
    ret
