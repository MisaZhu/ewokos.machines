#include <kernel/irq.h>
#include <kernel/kernel.h>
#include <kernel/hw_info.h>
#include <kernel/core.h>
#include "timer_arch.h"
#include <gic.h>

void irq_arch_init(void) {
	gic_init(MMIO_BASE + 0x139000, MMIO_BASE + 0x13A000);
}

inline uint32_t irq_get(uint32_t* irq_raw) {
	int ack = gic_get_irq();
	int irqno = ack & 0x3FF;
	*irq_raw = irqno;

	int core = get_core_id();//ack & (~0x3FF);
	//if(irqno == 0)
	//	printf("%s %d %d\n", __func__, core, irqno);
	if(irqno == 27){
		return IRQ_TIMER0;
	}else if(irqno == 0){
		return IRQ_IPI;
	}
	return 0;
}

inline void irq_eoi(uint32_t irq_raw) {
	gic_eoi(irq_raw);
}

inline void irq_enable(uint32_t irq) {
	if(irq == IRQ_TIMER0)
		gic_irq_enable(0, 27);
}

inline void irq_enable_core(uint32_t core, uint32_t irq) {
	if(irq == IRQ_TIMER0)
		gic_irq_enable(core, 27);
}

inline void irq_clear_core(uint32_t core, uint32_t irq) {

}

inline void irq_clear(uint32_t irq) {

}

void irq_disable(uint32_t irq) {
	(void)irq;
}
