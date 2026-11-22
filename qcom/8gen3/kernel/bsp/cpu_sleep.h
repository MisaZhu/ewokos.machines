/* SC8280XP secondary-core PSCI CPU_SUSPEND (core-level power-down).
 *
 * User space (powerd) opens and closes the gate for the secondary cores
 * through SYS_PLATFORM_CTL(Q8B_NPU_CPU_SLEEP); while it is open, a core's
 * idle hook parks it with PSCI CPU_SUSPEND64 instead of a plain WFI.  Core 0
 * never suspends: it drives the system tick and the polled wake sources
 * (power key / lid).  A parked core wakes exactly like a WFI core does: on
 * any interrupt targeted at it, which for a secondary is the scheduler IPI
 * (SGI).
 *
 * The gate is shut at boot, so a kernel carrying this code boots and idles
 * exactly as before until user space opts in.
 *
 * Resume translation.  PSCI re-enters at the trampoline with the MMU off, so
 * the trampoline page is identity-mapped - but only in the kernel VM
 * (arch_vm()).  Process VMs, including each core's idle proc, share just the
 * kernel half and have no such mapping, and an idle core normally still has
 * its idle proc's VM in TTBR0.  The trampoline therefore always comes up on
 * the kernel VM, and q8b_cpu_suspend() puts the live TTBRs back (and drops
 * the local TLB) once it is running at the kernel VA again.
 *
 * Save-area layout shared between cpu_sleep.c and cpu_sleep_a.S.  One
 * cache-line-aligned block per core; the suspend path writes it through the
 * kernel VA and flushes it to DRAM by VA, and the resume trampoline reads it
 * back through the PA with the MMU still off.
 */
#ifndef Q8B_CPU_SLEEP_H
#define Q8B_CPU_SLEEP_H

/* Word indices into the per-core save area (each slot is 8 bytes). */
#define CS_SCTLR    0   /* SCTLR_EL1   */
#define CS_TTBR0    1   /* TTBR0_EL1 for the trampoline (kernel VM) */
#define CS_TTBR1    2   /* TTBR1_EL1 for the trampoline (kernel VM) */
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
#define CS_CNTKCTL  26  /* CNTKCTL_EL1: EL0 counter access + event stream */
#define CS_MDSCR    27  /* MDSCR_EL1   */
#define CS_D8       28  /* d8..d15 saved at 28..35 (AAPCS64 callee-saved) */
#define CS_WORDS    40  /* 320 bytes: a whole number of 64-byte cache lines */

/* q8b_cpu_do_suspend() result after a real power-down/resume cycle.  PSCI
 * itself returns 0 (woke without losing context) or a negative error. */
#define CS_RESUMED  1

#ifndef __ASSEMBLER__

#include <stdint.h>

/* Q8B_NPU_CPU_SLEEP: arg != 0 opens the gate for every secondary core, 0
 * closes it and sends each of them an IPI.  Returns 0, or -1 when the resume
 * trampoline is not identity-mapped in the kernel VM (the gate stays shut). */
int q8b_cpu_sleep_ctl(uint32_t arg);

/* Q8B_NPU_CPU_SLEEP_STATS: a core mask (bit N = core N) selected by arg, see
 * Q8B_CPU_SLEEP_STATS_* in q8b_npu.h.  TW220 has no wired kernel console, so
 * this is the only view of the resume path. */
uint64_t q8b_cpu_sleep_stat(uint32_t arg);

/* Called from the core-0 tick: re-kicks secondaries that are idling in the
 * halt loop's WFI (reached by a blocking syscall, not by an IRQ) while the
 * gate is open, so they pass through the idle hook and park. */
void q8b_cpu_sleep_tick(void);

/* Idle hook replacement used by arch_irq_idle() on Q8B_BOARD builds. */
void q8b_arch_irq_idle(void);

/* Resume trampoline entry (cpu_sleep_a.S).  Entered with the MMU off at its
 * physical address, x0 = save-area PA (the PSCI context_id).  Its page is
 * identity-mapped in arch_vm() so re-enabling the MMU does not break the
 * instruction fetch. */
extern char q8b_cpu_resume_asm[];

#endif /* __ASSEMBLER__ */
#endif /* Q8B_CPU_SLEEP_H */
