#include <kernel/irq.h>
#include <kernel/kernel.h>
#include <kernel/hw_info.h>
#include <kernel/core.h>
#include "timer_arch.h"
#include <gic.h>

void irq_init_arch(void) {
	gic_init(MMIO_BASE + 0x581000, MMIO_BASE + 0x582000);
}

inline uint32_t irq_get_arch(void) {
	uint32_t irqno = gic_get_irq() & 0x3FF;
	return irqno;
}

inline uint32_t irq_get_unified_arch(uint32_t irqno) {
	if(irqno == 27){
		irqno = IRQ_TIMER0;
	}else if(irqno == 0){
		irqno = IRQ_IPI;
	}
	return irqno;
}

inline void irq_eoi_arch(uint32_t irq_raw) {
	gic_eoi(irq_raw);
}

inline void irq_enable_arch(uint32_t irq) {
	if(irq == IRQ_TIMER0)
		irq = 27;
	gic_irq_enable(0, irq);
}

inline void irq_enable_core_arch(uint32_t core, uint32_t irq) {
	if(irq == IRQ_TIMER0)
		irq = 27;
	gic_irq_enable(core, irq);
}

inline void irq_clear_core_arch(uint32_t core, uint32_t irq) {

}

inline void irq_clear_arch(uint32_t irq) {

}

void irq_disable_arch(uint32_t irq) {
	if(irq == IRQ_TIMER0)
		irq = 27;
	gic_irq_disable(0, irq);
}
