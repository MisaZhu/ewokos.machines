#include <kernel/core.h>
#include <kernel/system.h>
#include <kernel/smp/ipi.h>
#include "hw_arch.h"

#ifdef KERNEL_SMP

extern void timer_enable_el0_cnt_access(void);
extern void timer_disable_local(void);

void cpu_core_ready(uint32_t core_id) {
    /* UEFI previously schedules work on core 1. Do not inherit an armed,
     * expired virtual timer: EwokOS drives its system tick on core 0 only. */
    timer_disable_local();
    gic_v3_core_init(core_id);
    set_vector_table((ewokos_addr_t)&interrupt_table_start);
    timer_enable_el0_cnt_access();
    ipi_enable(core_id);
    __irq_enable();

}

inline uint32_t get_cpu_cores(void) {
    return Q8B_CORES;
}

/* PSCI CPU_ON (SMCCC SMC, arm64 calling convention). */
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
    if (core_id >= Q8B_CORES)
        return;

    /* SC8280XP cpu@0/100/.../700: MPIDR Aff1 == logical core, Aff0 == 0. */
    uint64_t target_mpidr = (uint64_t)core_id << 8;
    arm_smccc_res_t res;

    flush_dcache();
    arm_smccc_smc(PSCI_CPU_ON_AARCH64, target_mpidr,
            (uint64_t)__entry, 0, 0, 0, 0, 0, &res);
    if (res.a0 != 0) {
        q8b_dbg_puts("q8b: PSCI CPU_ON core ");
        q8b_dbg_dec(core_id);
        q8b_dbg_puts(" failed: ");
        q8b_dbg_hex(res.a0);
        q8b_dbg_puts("\n");
    }
}

#endif
