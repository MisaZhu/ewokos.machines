/* SC8280XP secondary-core PSCI CPU_SUSPEND: C side.
 *
 * See cpu_sleep.h for the contract and the save-area layout.  The suspend
 * SMC is issued with the MMU still on: the firmware owns the power-down
 * transition.  The kernel .text has no identity alias, so disabling the MMU
 * here before the SMC would fault the very next fetch; leaving it on is both
 * safe and the standard PSCI calling model.  On resume the firmware returns
 * with the MMU off at the resume trampoline PA, which rebuilds the
 * translation from the saved context (cpu_sleep_a.S).
 *
 * Concurrency.  The kernel has no atomics, so every per-core field has a
 * single writer: the controlling core (the Q8B_NPU_CPU_SLEEP caller, under
 * the kernel lock) owns on/epoch, the core itself owns everything else.
 */
#include <kernel/core.h>
#include <kernel/kernel.h>
#include <kernel/proc.h>
#include <kernel/system.h>
#include <kernel/smp/ipi.h>
#include <dev/timer.h>
#include <mm/mmu.h>
#include <stdint.h>
#include <q8b_npu.h>

#include "hw_arch.h"
#include "cpu_sleep.h"

#ifdef KERNEL_SMP

extern void timer_disable_local(void);
extern void __flush_tlb_local(void);

typedef struct {
    uint64_t a0;
    uint64_t a1;
    uint64_t a2;
    uint64_t a3;
} arm_smccc_res_t;

extern void arm_smccc_smc(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
        uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
        arm_smccc_res_t* res);

#define PSCI_FEATURES            0x8400000AUL
#define PSCI_SET_SUSPEND_MODE    0x8400000FUL
#define PSCI_CPU_SUSPEND_AARCH64 0xC4000001UL
#define PSCI_FEAT_OSI            (1u << 0)  /* CPU_SUSPEND feature flags */
#define PSCI_SUSPEND_MODE_OSI    1

/* A core that keeps getting PSCI errors stops trying until the next enable
 * request, and idles in WFI like before. */
#define CS_MAX_FAILS    8u
/* Period of the core-0 sweep that pushes halt-loop idlers into the hook. */
#define CS_SWEEP_USEC   200000u

typedef struct {
    /* written by the controlling core */
    volatile uint32_t on;           /* gate: suspend instead of WFI */
    volatile uint32_t epoch;        /* bumped by every enable request */
    /* written by the owning core */
    volatile uint32_t giveup_epoch; /* epoch that hit CS_MAX_FAILS */
    volatile uint32_t parked;       /* inside the suspend call */
    volatile uint32_t fail_streak;
    volatile uint32_t resumed;      /* powered down and came back, ever */
} __attribute__((aligned(64))) cs_core_t;

static cs_core_t _cs[Q8B_CORES];

/* One cache-line-aligned save block per core (CS_WORDS * 8 bytes). */
static uint64_t _save[Q8B_CORES][CS_WORDS] __attribute__((aligned(64)));

/* Set while any core may suspend; gates the tick sweep. */
static volatile uint32_t _cs_active;

/* One-time firmware/mapping check, run by the first enable request. */
#define CS_PREP_NONE     0u
#define CS_PREP_OK       1u
#define CS_PREP_NO_IDMAP 2u
static uint32_t _prep;

#define RD_SYSREG(name) (__extension__ ({ uint64_t _v; \
    __asm__ volatile("mrs %0, " #name : "=r"(_v)); _v; }))
#define WR_SYSREG(name, v) do { uint64_t _v = (v); \
    __asm__ volatile("msr " #name ", %0" : : "r"(_v)); } while (0)

static int32_t psci_call(uint64_t fn, uint64_t a1) {
    arm_smccc_res_t res;
    arm_smccc_smc(fn, a1, 0, 0, 0, 0, 0, 0, &res);
    return (int32_t)res.a0;
}

/* The host Linux on this board reports "failed to set PC mode: -3" and then
 * runs its cpuidle states in OS-initiated mode, so platform-coordinated
 * CPU_SUSPEND is not a mode this firmware is exercised in.  Select OSI the
 * same way; only the core-level state is ever requested, which needs no
 * cluster bookkeeping from the OS.  A refusal is not fatal. */
static int cs_prepare(void) {
    if (_prep != CS_PREP_NONE)
        return _prep == CS_PREP_OK ? 0 : -1;

    ewokos_addr_t tramp_pa = V2P((ewokos_addr_t)q8b_cpu_resume_asm);
    if (resolve_phy_address(_kernel_info.kernel_vm, tramp_pa) != tramp_pa) {
        _prep = CS_PREP_NO_IDMAP;
        return -1;
    }

    int32_t feat = psci_call(PSCI_FEATURES, PSCI_CPU_SUSPEND_AARCH64);
    if (feat >= 0 && (feat & PSCI_FEAT_OSI) != 0)
        (void)psci_call(PSCI_SET_SUSPEND_MODE, PSCI_SUSPEND_MODE_OSI);
    _prep = CS_PREP_OK;
    return 0;
}

/* Would the idle hook of this core suspend right now?  Read-only. */
static int cs_core_pending(uint32_t core, uint32_t* epoch_out) {
    cs_core_t* c = &_cs[core];
    uint32_t on = c->on;
    uint32_t epoch = c->epoch;

    if (epoch_out != NULL)
        *epoch_out = epoch;
    return on && c->giveup_epoch != epoch;
}

int q8b_cpu_sleep_ctl(uint32_t arg) {
    uint32_t self = get_core_id();

    if (arg == 0) {
        _cs_active = 0;
        for (uint32_t c = 1; c < Q8B_CORES; c++)
            _cs[c].on = 0;
        /* Pull every parked core back now instead of letting the first wake
         * IPC to each one pay the resume latency.  Cores that are merely in
         * WFI take the SGI and re-idle. */
        for (uint32_t c = 1; c < Q8B_CORES; c++) {
            if (c != self && _cpu_cores[c].actived)
                ipi_send(c);
        }
        return 0;
    }

    if (cs_prepare() != 0)
        return -1;

    for (uint32_t c = 1; c < Q8B_CORES; c++) {
        _cs[c].epoch++;
        _cs[c].on = 1;
    }
    _cs_active = 1;
    /* The idle hook only runs at the tail of an interrupt: a core already
     * sitting in WFI would otherwise stay there until something else woke
     * it.  Cores busy running a task ignore the kick. */
    for (uint32_t c = 1; c < Q8B_CORES; c++) {
        if (c != self && _cpu_cores[c].actived)
            ipi_send(c);
    }
    return 0;
}

uint64_t q8b_cpu_sleep_stat(uint32_t arg) {
    uint32_t mask = 0;

    if (arg != Q8B_CPU_SLEEP_STATS_RESUMED &&
            arg != Q8B_CPU_SLEEP_STATS_PARKED)
        return (uint64_t)Q8B_NPU_PAS_INVALID;
    for (uint32_t c = 1; c < Q8B_CORES; c++) {
        if (arg == Q8B_CPU_SLEEP_STATS_PARKED ?
                _cs[c].parked : _cs[c].resumed)
            mask |= 1u << c;
    }
    return mask;
}

void q8b_cpu_sleep_tick(void) {
    static uint64_t last;

    if (!_cs_active)
        return;
    uint64_t now = timer_read_sys_usec();
    if (now - last < CS_SWEEP_USEC)
        return;
    last = now;
    for (uint32_t c = 1; c < Q8B_CORES; c++) {
        if (!cs_core_pending(c, NULL) || _cs[c].parked ||
                !_cpu_cores[c].actived)
            continue;
        proc_t* p = get_current_core_proc(c);
        if (p != NULL && p->is_core_idle_proc)
            ipi_send(c);
    }
}

/* Asm helper (cpu_sleep_a.S): saves x19-x30/d8-d15/SP, flushes the save
 * block, issues CPU_SUSPEND64 with the MMU on.  Returns CS_RESUMED after a
 * power-down/resume cycle, else the PSCI result. */
extern int64_t q8b_cpu_do_suspend(uint64_t save_va, uint64_t save_pa,
        uint64_t resume_pa, uint64_t core);

static int32_t q8b_cpu_suspend(uint32_t core) {
    uint64_t* s = _save[core];
    uint64_t daif = RD_SYSREG(daif);
    /* The trampoline must come up on the kernel VM: that is the only VM
     * holding its identity mapping.  Keep the live pair here instead. */
    uint64_t ttbr0 = RD_SYSREG(ttbr0_el1);
    uint64_t ttbr1 = RD_SYSREG(ttbr1_el1);
    uint64_t kvm = (uint64_t)V2P((ewokos_addr_t)_kernel_info.kernel_vm);

    __asm__ volatile("msr daifset, #0xf");
    s[CS_SCTLR]   = RD_SYSREG(sctlr_el1);
    s[CS_TTBR0]   = kvm;
    s[CS_TTBR1]   = kvm;
    s[CS_TCR]     = RD_SYSREG(tcr_el1);
    s[CS_MAIR]    = RD_SYSREG(mair_el1);
    s[CS_VBAR]    = RD_SYSREG(vbar_el1);
    s[CS_CPACR]   = RD_SYSREG(cpacr_el1);
    s[CS_ACTLR]   = RD_SYSREG(actlr_el1);
    s[CS_TPIDR]   = RD_SYSREG(tpidr_el0);
    s[CS_CNTV]    = RD_SYSREG(cntv_ctl_el0);
    s[CS_CTXID]   = RD_SYSREG(contextidr_el1);
    s[CS_CNTKCTL] = RD_SYSREG(cntkctl_el1);
    s[CS_MDSCR]   = RD_SYSREG(mdscr_el1);

    uint64_t save_va = (uint64_t)s;
    uint64_t save_pa = (uint64_t)V2P((ewokos_addr_t)s);
    uint64_t resume_pa = (uint64_t)V2P((ewokos_addr_t)q8b_cpu_resume_asm);
    int32_t ret = (int32_t)q8b_cpu_do_suspend(save_va, save_pa, resume_pa, core);

    if (ret == CS_RESUMED) {
        /* Running on the kernel VM (ASID 0).  Go back to the VM this core
         * was idling on, and drop the trampoline's global identity entry so
         * it can never translate a user address of a later task. */
        WR_SYSREG(ttbr1_el1, ttbr1);
        WR_SYSREG(ttbr0_el1, ttbr0);
        __asm__ volatile("isb");
        __flush_tlb_local();
    }
    WR_SYSREG(daif, daif);
    return ret;
}

/* Called from the resume trampoline once the MMU is back on.  Re-run the
 * per-core GIC CPU interface + redistributor init (idempotent, mirrors
 * cpu_core_ready) and keep the local virtual timer stopped: EwokOS drives
 * the system tick on core 0 only.  Nothing here may block or print: the
 * core is still on the kernel VM with every exception masked. */
void q8b_cpu_resume_core(uint32_t core) {
    timer_disable_local();
    gic_v3_core_init(core);
}

void q8b_arch_irq_idle(void) {
    uint32_t core = get_core_id();
    uint32_t epoch = 0;

    /* Core 0 carries the tick and the polled wake sources: never suspend. */
    if (core == 0 || core >= Q8B_CORES || !cs_core_pending(core, &epoch)) {
        wfi();
        return;
    }

    cs_core_t* c = &_cs[core];
    c->parked = 1;
    int32_t ret = q8b_cpu_suspend(core);
    c->parked = 0;

    if (ret == CS_RESUMED) {
        c->resumed = 1;
        c->fail_streak = 0;
        return;
    }
    if (ret == 0) {
        c->fail_streak = 0;
    } else if (++c->fail_streak >= CS_MAX_FAILS) {
        c->fail_streak = 0;
        c->giveup_epoch = epoch;
    }
    /* The core never powered down and its registers are intact: idle the
     * old way.  A wake interrupt that is already pending falls through. */
    wfi();
}

#endif /* KERNEL_SMP */
