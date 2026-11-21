#include <kernel/core.h>
#include <kernel/system.h>
#include <kernel/smp/ipi.h>
#include "hw_arch.h"

#ifdef KERNEL_SMP

extern void timer_enable_el0_cnt_access(void);
extern void timer_disable_local(void);

void cpu_core_ready(uint32_t core_id) {
    /* UEFI/TF-A may leave an armed virtual timer on this core. EwokOS
     * drives its system tick on the boot core only. */
    timer_disable_local();
    gic_v3_core_init(core_id);
    set_vector_table((ewokos_addr_t)&interrupt_table_start);
    timer_enable_el0_cnt_access();
    ipi_enable(core_id);
    __irq_enable();
}

inline uint32_t get_cpu_cores(void) {
    return ORIN_CORES;
}

/* PSCI CPU_ON (SMCCC SMC, arm64 calling convention). Jetson UEFI hands off
 * with TF-A BL31 resident at EL3, which implements PSCI. */
#define PSCI_CPU_ON_AARCH64 0xC4000003UL

typedef struct {
    uint64_t a0;
    uint64_t a1;
    uint64_t a2;
    uint64_t a3;
} arm_smccc_res_t;

extern void arm_smccc_smc(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
        uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
        arm_smccc_res_t* res);

extern char __entry[];

void start_core(uint32_t core_id) {
    if (core_id >= ORIN_CORES)
        return;

    /* Orin Nano is 4+2 across two clusters (Aff2=cluster); cluster1 cores
     * are 0x10200/0x10300, so the PSCI target MPIDR must come from the
     * table, not core_id << 8. */
    uint64_t target_mpidr = orin_core_mpidr[core_id];
    arm_smccc_res_t res;

    flush_dcache();
    arm_smccc_smc(PSCI_CPU_ON_AARCH64, target_mpidr,
            (uint64_t)__entry, 0, 0, 0, 0, 0, &res);
    orin_dbg_puts("orin: PSCI CPU_ON core ");
    orin_dbg_dec(core_id);
    orin_dbg_puts(" mpidr=");
    orin_dbg_hex(target_mpidr);
    if (res.a0 != 0) {
        orin_dbg_puts(" failed: ");
        orin_dbg_hex(res.a0);
    } else {
        orin_dbg_puts(" ok");
    }
    orin_dbg_puts("\n");
}

#endif