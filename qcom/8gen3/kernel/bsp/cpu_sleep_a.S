/* SC8280XP secondary-core PSCI CPU_SUSPEND: assembly side.
 *
 * q8b_cpu_do_suspend  - save the caller's callee-saved frame, flush the save
 *                       block to DRAM, issue CPU_SUSPEND64 with the MMU on.
 * q8b_cpu_resume_asm  - PSCI resume entry (MMU off, x0 = save-area PA).
 *                       Rebuild the translation from the saved context and
 *                       return through the untouched idle-proc frame.
 *
 * The resume trampoline is aligned to 64K and kept (with its literal pool)
 * inside a single page so arch_vm() can identity-map just that page: with
 * the MMU off the core fetches it at PA, and re-enabling the MMU keeps that
 * fetch valid because PA == VA there.  See cpu_sleep.h for the save layout.
 */
#include "cpu_sleep.h"

.section .text

/* int q8b_cpu_do_suspend(save_va=x0, save_pa=x1, resume_pa=x2, core=x3)
 * Returns CS_RESUMED after a power-down/resume cycle, otherwise the PSCI
 * result: 0 if the core woke without losing context, negative on error. */
.global q8b_cpu_do_suspend
.type q8b_cpu_do_suspend, %function
q8b_cpu_do_suspend:
    mov x9, x0                          /* x9 = save-area VA base */
    stp x19, x20, [x9, #CS_X19*8]
    stp x21, x22, [x9, #(CS_X19+2)*8]
    stp x23, x24, [x9, #(CS_X19+4)*8]
    stp x25, x26, [x9, #(CS_X19+6)*8]
    stp x27, x28, [x9, #(CS_X19+8)*8]
    stp x29, x30, [x9, #(CS_X19+10)*8]
    stp d8,  d9,  [x9, #CS_D8*8]
    stp d10, d11, [x9, #(CS_D8+2)*8]
    stp d12, d13, [x9, #(CS_D8+4)*8]
    stp d14, d15, [x9, #(CS_D8+6)*8]
    mov x10, sp
    str x10, [x9, #CS_SP*8]
    str x3,  [x9, #CS_CORE*8]
    str x0,  [x9, #CS_SAVEVA*8]
    /* Park the parameters in callee-saved regs across the flush call. */
    mov x20, x0                         /* save_va base */
    mov x21, x2                         /* resume_pa */
    mov x22, x1                         /* save_pa (context_id) */
    /* The trampoline reads this block through the PA with the MMU off, so
     * it must be in DRAM, not in this core's cache.  By VA, this block only:
     * the firmware cleans the rest of the cache as part of the power-down. */
    add x1, x0, #CS_WORDS*8
    bl __dcache_flush_poc_range
    /* CPU_SUSPEND64(power_state, entry_point, context_id), MMU left on:
     * the firmware performs the power-down MMU-off transition itself. */
    ldr x0, =0xC4000001
    ldr x1, =0x40000004
    mov x2, x21                         /* entry point PA */
    mov x3, x22                         /* context_id = save-area PA */
    smc #0
    /* The call returned, so the core never powered down and every register
     * but the result is intact.  Restore the caller's callee-saved frame
     * (x20/x21/x22 were repurposed, x30 by the flush) and hand the PSCI
     * result back in x0; SP and the MMU are exactly as on entry. */
    mov x9, x20
    ldp x19, x20, [x9, #CS_X19*8]
    ldp x21, x22, [x9, #(CS_X19+2)*8]
    ldp x23, x24, [x9, #(CS_X19+4)*8]
    ldp x25, x26, [x9, #(CS_X19+6)*8]
    ldp x27, x28, [x9, #(CS_X19+8)*8]
    ldp x29, x30, [x9, #(CS_X19+10)*8]
    ret
.ltorg

/* PSCI resume entry.  MMU off, x0 = save-area PA.  This page is identity
 * mapped in the kernel VM, which is what CS_TTBR0/CS_TTBR1 hold, so it
 * executes seamlessly at PA before and after the MMU comes on. */
.align 16
.global q8b_cpu_resume_asm
.type q8b_cpu_resume_asm, %function
q8b_cpu_resume_asm:
    msr daifset, #0xf                   /* keep exceptions masked */
    msr spsel, #1
    mov x19, x0                         /* save-area PA base (MMU off) */
    ldr x20, [x19, #CS_SAVEVA*8]        /* x20 = save VA, used once MMU on */
    /* Rebuild the translation and the sysregs lost on core power-down. */
    ldr x1, [x19, #CS_CPACR*8];   msr cpacr_el1, x1
    ldr x1, [x19, #CS_TTBR0*8];   msr ttbr0_el1, x1
    ldr x1, [x19, #CS_TTBR1*8];   msr ttbr1_el1, x1
    ldr x1, [x19, #CS_TCR*8];     msr tcr_el1, x1
    ldr x1, [x19, #CS_MAIR*8];    msr mair_el1, x1
    ldr x1, [x19, #CS_VBAR*8];    msr vbar_el1, x1
    ldr x1, [x19, #CS_ACTLR*8];   msr actlr_el1, x1
    ldr x1, [x19, #CS_TPIDR*8];   msr tpidr_el0, x1
    ldr x1, [x19, #CS_CNTV*8];    msr cntv_ctl_el0, x1
    ldr x1, [x19, #CS_CTXID*8];   msr contextidr_el1, x1
    ldr x1, [x19, #CS_CNTKCTL*8]; msr cntkctl_el1, x1
    ldr x1, [x19, #CS_MDSCR*8];   msr mdscr_el1, x1
    isb
    ic iallu
    tlbi vmalle1                        /* this core only: nobody else lost state */
    dsb nsh
    isb
    ldr x1, [x19, #CS_SCTLR*8]
    msr sctlr_el1, x1                   /* MMU/caches on; PC==PA stays valid */
    isb
    /* MMU on: x20 addresses the save area through the kernel VA. */
    ldr x1, [x20, #CS_SP*8]
    mov sp, x1
    ldr x0, [x20, #CS_CORE*8]
    ldr x1, =q8b_cpu_resume_core
    blr x1                              /* re-init GIC CPU IF + local timer */
    /* Restore the exact callee-saved frame the suspend call saved, then ret
     * into q8b_cpu_suspend -> q8b_arch_irq_idle -> irq_handler tail (eret). */
    ldp d8,  d9,  [x20, #CS_D8*8]
    ldp d10, d11, [x20, #(CS_D8+2)*8]
    ldp d12, d13, [x20, #(CS_D8+4)*8]
    ldp d14, d15, [x20, #(CS_D8+6)*8]
    ldr x19, [x20, #CS_X19*8]
    ldp x21, x22, [x20, #(CS_X19+2)*8]
    ldp x23, x24, [x20, #(CS_X19+4)*8]
    ldp x25, x26, [x20, #(CS_X19+6)*8]
    ldp x27, x28, [x20, #(CS_X19+8)*8]
    ldp x29, x30, [x20, #(CS_X19+10)*8]
    ldr x1, [x20, #CS_SP*8]
    mov sp, x1
    ldr x20, [x20, #(CS_X19+1)*8]       /* restore x20 last (was the base) */
    mov x0, #CS_RESUMED                 /* tell C the context was really lost */
    ret
.ltorg
