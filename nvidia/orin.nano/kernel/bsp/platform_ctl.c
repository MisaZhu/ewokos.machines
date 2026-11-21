#include <kernel/kernel.h>
#include <stdint.h>
#include "hw_arch.h"
#include "machine_conf.h"

/* PSCI SYSTEM_RESET (SMC32 fast call), implemented by TF-A BL31 at EL3.
 * Resets the whole SoC; used by /bin/reboot and the automation pipeline.
 * The hardware SBSA watchdog is not usable on Orin (NVIDIA confirms it may
 * not be supported, and userspace access to its frames raises an SError);
 * board recovery during bring-up uses the Pico GPIO hardware reset. */
#define PSCI_SYSTEM_RESET 0x84000009UL

typedef struct {
    uint64_t a0, a1, a2, a3;
} arm_smccc_res_t;

extern void arm_smccc_smc(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
        uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
        arm_smccc_res_t* res);

static void do_reset(void) {
    arm_smccc_res_t res;
    orin_dbg_puts("orin: PSCI SYSTEM_RESET\n");
    __asm__ volatile("dsb sy" ::: "memory");
    arm_smccc_smc(PSCI_SYSTEM_RESET, 0, 0, 0, 0, 0, 0, 0, &res);
    /* A successful reset never returns. */
    orin_dbg_puts("orin: SYSTEM_RESET returned a0=");
    orin_dbg_hex(res.a0);
    orin_dbg_puts(" (failed)\n");
}

ewokos_addr_t platform_control_arch(ewokos_addr_t op, ewokos_addr_t arg,
        ewokos_addr_t size) {
    (void)arg; (void)size;
    switch (op) {
    case ORIN_PLATFORM_RESET:
        do_reset();
        break;
    }
    return (ewokos_addr_t)-1;
}

/* Boot-time SMP topology probe: print the boot core MPIDR and query PSCI
 * AFFINITY_INFO for each candidate core to find which are actually present
 * (the CCPLEX_CLUSTER_DISABLE fuse gates cores; targeting a fused-off core
 * via CPU_ON raises a RAS error).  AFFINITY_INFO a0: 0=on 1=off 2=invalid,
 * negative = PSCI error. */
#define PSCI_AFFINITY_INFO 0xC4000004UL
void orin_probe_cores(void) {
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    orin_dbg_puts("orin smp: boot MPIDR=");
    orin_dbg_hex(mpidr & 0xFFFFFFULL);
    orin_dbg_puts("\n");
    const uint64_t cand[] = {
        0x0, 0x100, 0x200, 0x300,
        0x10000, 0x10100, 0x10200, 0x10300,
    };
    for (unsigned i = 0; i < sizeof(cand) / sizeof(cand[0]); i++) {
        arm_smccc_res_t res;
        arm_smccc_smc(PSCI_AFFINITY_INFO, cand[i], 0, 0, 0, 0, 0, 0, &res);
        orin_dbg_puts("orin smp: aff ");
        orin_dbg_hex(cand[i]);
        orin_dbg_puts(" = ");
        orin_dbg_hex(res.a0);
        orin_dbg_puts("\n");
    }
}