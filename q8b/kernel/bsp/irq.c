#include <kernel/irq.h>
#include <kernel/kernel.h>
#include <kernel/system.h>
#include <kernel/hw_info.h>
#include <mm/mmu.h>
#include <stdint.h>
#include "hw_arch.h"

/*
 * Q8B interrupt controller glue: GICv3 (see bsp/gic_v3.c) + ARM generic
 * timer.  The platform layer still owns the exception vectors and the
 * generic IRQ dispatch.
 */


#ifdef Q8B_QEMU_TEST
/* QEMU virt: GICv2 (distributor 0x08000000, CPU interface 0x08010000). */
extern void gic_init(void* gicd, void* gicc);
extern void gic_irq_enable(int core_id, int irqno);
extern void gic_irq_disable(int core_id, int irqno);
extern int  gic_get_irq(void);
extern void gic_eoi(uint32_t intn);

void irq_init_arch(void) {
    gic_init((void*)(MMIO_BASE + Q8B_GIC_WIN_OFF),
             (void*)(MMIO_BASE + Q8B_GIC_WIN_OFF +
                 (Q8B_GICC_PHY - Q8B_GICD_PHY)));
    for (int i = 0; i < 1020; i++)
        gic_irq_disable(0, i);
    set_vector_table((ewokos_addr_t)&interrupt_table_start);
}

inline uint32_t irq_get_arch(void) {
    return (uint32_t)gic_get_irq() & 0x3FF;
}

inline uint32_t irq_get_unified_arch(uint32_t irqno) {
    if (irqno == Q8B_GIC_TIMER_IRQ)
        return IRQ_TIMER0;
    if (irqno == Q8B_GIC_IPI_IRQ)
        return IRQ_IPI;
    return irqno;
}

inline void irq_eoi_arch(uint32_t irq_raw) {
    gic_eoi(irq_raw);
}

static uint32_t irq_enable_flag = 0;

inline void irq_enable_arch(uint32_t irq) {
    if (irq == IRQ_TIMER0) {
        if (irq_enable_flag & IRQ_TIMER0)
            return;
        gic_irq_enable(0, Q8B_GIC_TIMER_IRQ);
        irq_enable_flag |= IRQ_TIMER0;
    } else if (irq > 0 && irq < 1020) {
        gic_irq_enable(0, (int)irq);
    }
}

inline void irq_enable_core_arch(uint32_t core, uint32_t irq) {
    if (irq == IRQ_TIMER0)
        gic_irq_enable((int)core, Q8B_GIC_TIMER_IRQ);
    else if (irq > 0 && irq < 1020)
        gic_irq_enable((int)core, (int)irq);
}

inline void irq_clear_core_arch(uint32_t core, uint32_t irq) {
    (void)core; (void)irq;
}

inline void irq_clear_arch(uint32_t irq) {
    (void)irq;
}

void irq_disable_arch(uint32_t irq) {
    if (irq > 0 && irq < 1020)
        gic_irq_disable(0, (int)irq);
}

#else
void irq_init_arch(void) {
    gic_v3_init();

    /* Firmware may have left arbitrary SPIs enabled; start from a clean
     * slate.  SGIs/PPIs (SGI0, timer PPI27) are enabled by gic_v3_init(). */
    for (int i = 32; i < 1020; i++)
        gic_v3_irq_disable(0, i);

    gic_v3_core_init(0);
    set_vector_table((ewokos_addr_t)&interrupt_table_start);
}

inline uint32_t irq_get_arch(void) {
    return gic_v3_get_irq();
}

inline uint32_t irq_get_unified_arch(uint32_t irqno) {
    if (irqno == Q8B_GIC_TIMER_IRQ)
        return IRQ_TIMER0;
    if (irqno == Q8B_GIC_IPI_IRQ)
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
        gic_v3_irq_enable(0, Q8B_GIC_TIMER_IRQ);
        irq_enable_flag |= IRQ_TIMER0;
    } else if (irq > 0 && irq < 1020) {
        gic_v3_irq_enable(0, (int)irq);
    }
}

inline void irq_enable_core_arch(uint32_t core, uint32_t irq) {
    if (irq == IRQ_TIMER0) {
        gic_v3_irq_enable((int)core, Q8B_GIC_TIMER_IRQ);
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

#endif /* Q8B_QEMU_TEST */
