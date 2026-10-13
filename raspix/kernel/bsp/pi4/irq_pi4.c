#include <kernel/irq.h>
#include <kernel/kernel.h>
#include <kernel/system.h>
#include <kernel/hw_info.h>
#include "../timer_arch.h"

static uint32_t irq_enable_flag = 0;
void irq_arch_init_pi4(void) {
    irq_enable_flag = 0;
    gic_init(MMIO_BASE + 0x1841000, MMIO_BASE + 0x1842000);
    for(int i = 0; i < 1022; i++){
        gic_irq_disable(0, i);
    }
    set_vector_table(&interrupt_table_start);
}

uint32_t irq_get_pi4(void) {
    int ack = gic_get_irq();
    int irqno = ack & 0x3FF;
    return irqno;
}

uint32_t irq_get_unified_pi4(uint32_t irqno) {
    if(irqno == 27){
        irqno = IRQ_TIMER0;
    }else if(irqno == 0){
        irqno = IRQ_IPI;
    }
    return irqno;
}

void irq_eoi_pi4(uint32_t irq_raw) {
    gic_eoi(irq_raw);
}

/*
 * Raw peripheral INTIDs (a GIC INTID below PI4_GIC_IRQ_MAX, i.e. anything the
 * unified mapping in irq_get_unified_pi4 left untouched) are forwarded to a
 * user-space handler by the shared interrupt_send() path: the kernel disables
 * the line before injecting the handler and re-enables it from interrupt_end().
 * Both must really reach the GIC here -- BCM2711 peripheral interrupts are
 * level triggered, so with a no-op disable the line would re-fire straight
 * after EOI and the handler's main context would never make progress. The
 * user-space irq number IS the GIC INTID (32 + SPI), same contract as raspi5.
 */
#define PI4_GIC_IRQ_MAX 1020

static inline int pi4_gic_raw_irq(uint32_t irq) {
    return irq > 0 && irq < PI4_GIC_IRQ_MAX;
}

void irq_enable_pi4(int core, uint32_t irq) {
    if(irq == IRQ_TIMER0){
        if(irq & irq_enable_flag)
            return;
        gic_irq_enable(core, 27);
        irq_enable_flag |= irq;
    }
    else if(pi4_gic_raw_irq(irq)){
        gic_irq_enable(core, irq);
    }
}

void irq_disable_pi4(uint32_t irq) {
    if(pi4_gic_raw_irq(irq))
        gic_irq_disable(0, irq);
}
