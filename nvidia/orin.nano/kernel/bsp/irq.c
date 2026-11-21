#include <kernel/irq.h>
#include <kernel/kernel.h>
#include <kernel/system.h>
#include <kernel/hw_info.h>
#include <mm/mmu.h>
#include <stdint.h>
#include "hw_arch.h"

/*
 * Orin Nano interrupt controller glue: GICv3 (see bsp/gic_v3.c) + ARM
 * generic timer.  The platform layer still owns the exception vectors and
 * the generic IRQ dispatch.
 */

void irq_init_arch(void) {
    gic_v3_init();

    /* Firmware may have left arbitrary SPIs enabled; start from a clean
     * slate.  SGIs/PPIs (SGI0, timer PPI27) are enabled by gic_v3_init(). */
    for (int i = 32; i < 1020; i++)
        gic_v3_irq_disable(0, i);

    gic_v3_core_init(orin_core_id());
    set_vector_table((ewokos_addr_t)&interrupt_table_start);
}

inline uint32_t irq_get_arch(void) {
    return gic_v3_get_irq();
}

inline uint32_t irq_get_unified_arch(uint32_t irqno) {
    if (irqno == ORIN_GIC_TIMER_IRQ)
        return IRQ_TIMER0;
    if (irqno == ORIN_GIC_IPI_IRQ)
        return IRQ_IPI;
    return irqno;
}

inline void irq_eoi_arch(uint32_t irq_raw) {
    gic_v3_eoi(irq_raw);
}

static uint32_t irq_enable_flag = 0;

inline void irq_enable_arch(uint32_t irq) {
    if (irq == IRQ_TIMER0) {
        if (irq_enable_flag & IRQ_TIMER0)
            return;
        gic_v3_irq_enable(0, ORIN_GIC_TIMER_IRQ);
        irq_enable_flag |= IRQ_TIMER0;
    } else if (irq > 0 && irq < 1020) {
        gic_v3_irq_enable(0, (int)irq);
    }
}

inline void irq_enable_core_arch(uint32_t core, uint32_t irq) {
    if (irq == IRQ_TIMER0) {
        gic_v3_irq_enable((int)core, ORIN_GIC_TIMER_IRQ);
        irq_enable_flag |= IRQ_TIMER0;
    } else if (irq > 0 && irq < 1020) {
        gic_v3_irq_enable((int)core, (int)irq);
    }
}

inline void irq_clear_core_arch(uint32_t core, uint32_t irq) {
    (void)core;
    (void)irq;
}

inline void irq_clear_arch(uint32_t irq) {
    (void)irq;
}

void irq_disable_arch(uint32_t irq) {
    if (irq > 0 && irq < 1020)
        gic_v3_irq_disable(0, (int)irq);
}