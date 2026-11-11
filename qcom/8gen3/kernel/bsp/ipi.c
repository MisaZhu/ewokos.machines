#include <kernel/smp/ipi.h>
#include <kernel/hw_info.h>
#include "hw_arch.h"

/* GICv3 SGIs for cross-core scheduling kicks. */

static uint8_t _ipi_seen[Q8B_CORES];

void ipi_enable(uint32_t core_id) {
    gic_v3_irq_enable((int)core_id, Q8B_GIC_IPI_IRQ);
}

void ipi_send(uint32_t core_id) {
    gic_v3_send_sgi((int)core_id, Q8B_GIC_IPI_IRQ);
}

void ipi_clear(uint32_t core_id) {
    if (core_id < Q8B_CORES && !_ipi_seen[core_id]) {
        _ipi_seen[core_id] = 1;
    }
}
