/* SC8280XP secondary-core PSCI CPU_SUSPEND (aff0 power-down).
 *
 * powerd flips a runtime flag through SYS_PLATFORM_CTL(Q8B_NPU_CPU_SLEEP);
 * while it is set, the idle hook on every core except core 0 parks the core
 * with PSCI CPU_SUSPEND64 instead of a plain WFI.  Core 0 never suspends: it
 * drives the system tick and the polled wake sources (power key / lid), and
 * the host DT marks its local timer stopped.  Secondaries wake on the
 * scheduler IPI through the GIC.
 *
 * The flag defaults OFF, so a kernel carrying this code boots and idles
 * exactly as before until user space opts in.  Each leg is independently
 * reversible (see powerd).
 *
 * Save-area layout shared between cpu_sleep.c and cpu_sleep_a.S.  One
 * cache-line-aligned block per core; the suspend path writes it through the
 * kernel VA, flushes the D-cache to PoC, and the resume trampoline reads it
 * back through the PA with the MMU still off.
 */
#ifndef Q8B_CPU_SLEEP_H
#define Q8B_CPU_SLEEP_H

/* Word indices into the per-core save area (each slot is 8 bytes). */
#define CS_SCTLR    0   /* SCTLR_EL1   */
#define CS_TTBR0    1   /* TTBR0_EL1   */
#define CS_TTBR1    2   /* TTBR1_EL1   */
#define CS_TCR      3   /* TCR_EL1     */
#define CS_MAIR     4   /* MAIR_EL1    */
#define CS_VBAR     5   /* VBAR_EL1    */
#define CS_CPACR    6   /* CPACR_EL1   */
#define CS_ACTLR    7   /* ACTLR_EL1   */
#define CS_TPIDR    8   /* TPIDR_EL0   */
#define CS_CNTV     9   /* CNTV_CTL_EL0*/
#define CS_CTXID    10  /* CONTEXTIDR_EL1 */
#define CS_SP       11  /* SP at the suspend call (kernel VA) */
#define CS_X19      12  /* x19..x30 saved at 12..23 (x30 == LR) */
#define CS_CORE     24  /* logical core id */
#define CS_SAVEVA   25  /* kernel VA of this save area (for the resume path) */
#define CS_WORDS    32  /* 256 bytes: a whole number of 64-byte cache lines */

#ifndef __ASSEMBLER__

#include <stdint.h>

/* Runtime gate, toggled by Q8B_NPU_CPU_SLEEP.  0 = plain WFI (boot default). */
void q8b_cpu_sleep_enable(int on);

/* Bitmask of cores that completed at least one suspend/resume cycle
 * (bit N = core N; core 0 never suspends).  Read by Q8B_NPU_CPU_SLEEP_STATS:
 * TW220 has no wired kernel console, so this is the observable proof that
 * PSCI CPU_SUSPEND actually powers cores down and brings them back. */
uint32_t q8b_cpu_sleep_resumed_mask(void);

/* Idle hook replacement used by arch_irq_idle() on Q8B_BOARD builds. */
void q8b_arch_irq_idle(void);

/* Suspend the calling secondary core; returns 0 after a successful resume,
 * or a PSCI error code if the call was denied (the core never powered down).
 * Core 0 must not call this. */
int q8b_cpu_suspend(uint32_t core);

/* Resume trampoline entry (cpu_sleep_a.S).  Entered with the MMU off at its
 * physical address, x0 = save-area PA (the PSCI context_id).  Its page is
 * identity-mapped in arch_vm() so re-enabling the MMU does not break the
 * instruction fetch. */
extern char q8b_cpu_resume_asm[];

#endif /* __ASSEMBLER__ */
#endif /* Q8B_CPU_SLEEP_H */