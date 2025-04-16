#include <kernel/irq.h>
#include <kernel/kernel.h>
#include <kernel/hw_info.h>
#include <kernel/core.h>
#include "timer_arch.h"
#include <gic.h>

void irq_arch_init(void) {
	gic_init(MMIO_BASE + 0x580000);
}

inline uint32_t irq_get(void) {
	uint32_t irqno = gic_get_irq() & 0x3FF;
	if(irqno == 27){
		irqno = IRQ_TIMER0;
	}else if(irqno == 0){
		irqno = IRQ_IPI;
	}
	return irqno;
}

inline void irq_enable(uint32_t irq) {
	if(irq == IRQ_TIMER0)
		irq = 27;
	gic_irq_enable(0, irq);
}

void irq_disable(uint32_t irq) {
	if(irq == IRQ_TIMER0)
		irq = 27;
	gic_irq_disable(0, irq);
}
