/*
 * SMCCC SMC call wrapper for AArch64.
 *
 * arm_smccc_smc(w0..w7, struct arm_smccc_res *res)
 *
 * Makes an SMC #0 call with parameters in x0-x7.
 * Result is stored in *res (x8 = pointer to result struct).
 */
    .global arm_smccc_smc
    .type   arm_smccc_smc, %function

arm_smccc_smc:
    /*
     * Per AAPCS64, the 9th argument (struct arm_smccc_res *res) is passed
     * on the stack at [sp, #0], not in x8.  Read it after the SMC returns
     * so we don't have to worry about the secure monitor clobbering a
     * temporary register.
     */
    smc     #0
    ldr     x9, [sp]        /* res pointer = 9th argument from stack */
    /* result in x0-x3, store via res pointer in x9 */
    stp     x0, x1, [x9]
    stp     x2, x3, [x9, #16]
    ret
