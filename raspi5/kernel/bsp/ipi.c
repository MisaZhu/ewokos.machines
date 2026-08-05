#include <kernel/smp/ipi.h>
#include <kernel/hw_info.h>
#include "hw_arch.h"

/* GICv2 helpers from the aarch64 platform layer */
extern void gic_irq_enable(int core_id, int irqno);
extern void gic_gen_soft_irq(int core_id, int irq);

#define PI5_GIC_IPI_IRQ 0 /* SGI0 */

void ipi_enable(uint32_t core_id) {
	gic_irq_enable(core_id, PI5_GIC_IPI_IRQ);
}

void ipi_send(uint32_t core_id) {
	gic_gen_soft_irq(core_id, PI5_GIC_IPI_IRQ);
}

void ipi_clear(uint32_t core_id) {
}
