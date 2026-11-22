/* SC8280XP secondary-core PSCI CPU_SUSPEND: C side.
 *
 * See cpu_sleep.h for the contract and the save-area layout.  The suspend
 * SMC is issued with the MMU still on: TF-A owns the power-down transition
 * (it turns the MMU off through its own identity map, saves the secure
 * context and collapses the core).  The kernel .text has no identity alias,
 * so disabling the MMU here before the SMC would fault the very next fetch;
 * leaving it on is both safe and the standard PSCI calling model.  On resume
 * TF-A returns with the MMU off at the resume trampoline PA, which rebuilds
 * the translation from the saved context (cpu_sleep_a.S).
 */
#include <kernel/core.h>
#include <kernel/system.h>
#include <kernel/smp/ipi.h>
#include <mm/mmu.h>
#include <kprintf.h>
#include <stdint.h>

#include "hw_arch.h"
#include "cpu_sleep.h"

#ifdef KERNEL_SMP

extern void gic_v3_core_init(uint32_t core);
extern void timer_disable_local(void);

/* PSCI CPU_SUSPEND64 and the host DT idle-state power_state: aff0 power-down,
 * entry ~355 us / exit ~909 us / min-residency ~4 ms (linux idle-states node). */
#define PSCI_CPU_SUSPEND_AARCH64 0xC4000001UL
#define Q8B_POWER_STATE_AFF0_PD  0x40000004UL

/* Boot default OFF: the kernel idles with WFI until powerd opts in. */
static volatile int _cpu_sleep_enabled;

/* One cache-line-aligned save block per core (CS_WORDS * 8 bytes). */
static uint64_t _save[Q8B_CORES][CS_WORDS] __attribute__((aligned(64)));

/* First successful resume per core logs once (verification hook). */
static uint32_t _resumed_once;

void q8b_cpu_sleep_enable(int on) {
    _cpu_sleep_enabled = on ? 1 : 0;
    if (!on) {
        /* Pull every secondary core out of PSCI power-down NOW: a parked
         * core only leaves CPU_SUSPEND on a targeted interrupt, so without
         * this kick the first wake fan-out IPC to each core pays the resume
         * latency itself - and a core whose wake-up IPI races the suspend
         * entry can stall the caller for the full IPC timeout.  Cores that
         * are merely in WFI take the SGI and re-idle (flag is already off). */
        for (uint32_t c = 1; c < Q8B_CORES; c++)
            ipi_send(c);
    }
    printf("q8b cpu_sleep: %s\n", on ? "enabled" : "disabled");
}

uint32_t q8b_cpu_sleep_resumed_mask(void) {
    return _resumed_once;
}

#define RD_SYSREG(name) (__extension__ ({ uint64_t _v; \
    __asm__ volatile("mrs %0, " #name : "=r"(_v)); _v; }))

/* Asm helper (cpu_sleep_a.S): saves x19-x30/SP, flushes the D-cache, issues
 * CPU_SUSPEND64 with the MMU on.  Returns the PSCI result (0 == resumed). */
extern int q8b_cpu_do_suspend(uint64_t save_va, uint64_t save_pa,
        uint64_t resume_pa, uint64_t core);

int q8b_cpu_suspend(uint32_t core) {
    if (core == 0 || core >= Q8B_CORES)
        return -1;

    uint64_t* s = _save[core];
    s[CS_SCTLR]  = RD_SYSREG(sctlr_el1);
    s[CS_TTBR0]  = RD_SYSREG(ttbr0_el1);
    s[CS_TTBR1]  = RD_SYSREG(ttbr1_el1);
    s[CS_TCR]    = RD_SYSREG(tcr_el1);
    s[CS_MAIR]   = RD_SYSREG(mair_el1);
    s[CS_VBAR]   = RD_SYSREG(vbar_el1);
    s[CS_CPACR]  = RD_SYSREG(cpacr_el1);
    s[CS_ACTLR]  = RD_SYSREG(actlr_el1);
    s[CS_TPIDR]  = RD_SYSREG(tpidr_el0);
    s[CS_CNTV]   = RD_SYSREG(cntv_ctl_el0);
    s[CS_CTXID]  = RD_SYSREG(contextidr_el1);

    uint64_t save_va = (uint64_t)s;
    uint64_t save_pa = (uint64_t)V2P((ewokos_addr_t)s);
    uint64_t resume_pa = (uint64_t)V2P((ewokos_addr_t)q8b_cpu_resume_asm);
    return q8b_cpu_do_suspend(save_va, save_pa, resume_pa, core);
}

/* Called from the resume trampoline once the MMU is back on.  Re-run the
 * per-core GIC CPU interface + redistributor init (idempotent, mirrors
 * cpu_core_ready) and keep the local virtual timer stopped: EwokOS drives
 * the system tick on core 0 only.  TF-A restores the GIC context for aff0
 * power-down, but re-initialising is cheap and matches the CPU_ON path. */
void q8b_cpu_resume_core(uint32_t core) {
    timer_disable_local();
    gic_v3_core_init(core);
    if ((_resumed_once & (1u << core)) == 0) {
        _resumed_once |= (1u << core);
        printf("q8b cpu_sleep: core %u resumed\n", core);
    }
}

void q8b_arch_irq_idle(void) {
    uint32_t core = get_core_id();
    /* Core 0 carries the tick and the polled wake sources: never suspend. */
    if (!_cpu_sleep_enabled || core == 0) {
        wfi();
        return;
    }
    if (q8b_cpu_suspend(core) != 0)
        wfi();   /* denied/failed: fall back to a plain WFI, registers intact */
}

#endif /* KERNEL_SMP */