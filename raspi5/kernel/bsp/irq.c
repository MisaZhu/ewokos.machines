#include <kernel/irq.h>
#include <kernel/kernel.h>
#include <kernel/system.h>
#include <kernel/hw_info.h>
#include "timer_arch.h"
#include "hw_arch.h"

/* GICv2 helpers from the aarch64 platform layer */
extern void gic_init(void* gicd, void* gicc);
extern void gic_irq_enable(int core_id, int irqno);
extern void gic_irq_disable(int core_id, int irqno);
extern int  gic_get_irq(void);
extern void gic_eoi(uint32_t intn);
extern void gic_gen_soft_irq(int core_id, int irq);

/*
 * BCM2712 uses a GIC-400 (GICv2). The distributor/cpu interface sit at
 * the top of the peripheral window.
 */

#define PI5_GIC_TIMER_IRQ 27 /* CNTV PPI */
#define PI5_GIC_IPI_IRQ   0  /* SGI0     */

void pi5_dbg_puts(const char* s);

void irq_init_arch(void) {
    gic_init((void*)(MMIO_BASE + PI5_GICD_OFF),
             (void*)(MMIO_BASE + PI5_GICC_OFF));
    for(int i = 0; i < 1022; i++){
        gic_irq_disable(0, i);
    }
    set_vector_table(&interrupt_table_start);
}

inline uint32_t irq_get_arch(void) {
    int ack = gic_get_irq();
    int irqno = ack & 0x3FF;
    return irqno;
}

inline uint32_t irq_get_unified_arch(uint32_t irqno) {
    if(irqno == PI5_GIC_TIMER_IRQ){
        irqno = IRQ_TIMER0;
    }else if(irqno == PI5_GIC_IPI_IRQ){
        irqno = IRQ_IPI;
    }
    return irqno;
}

inline void irq_eoi_arch(uint32_t irq_raw) {
    gic_eoi(irq_raw);
}

static uint32_t irq_enable_flag = 0;

inline void irq_enable_arch(uint32_t irq) {
    if(irq & irq_enable_flag)
        return;

    if(irq == IRQ_TIMER0){
        gic_irq_enable(0, PI5_GIC_TIMER_IRQ);
        irq_enable_flag |= irq;
    }
}

inline void irq_enable_core_arch(uint32_t core, uint32_t irq) {
    if(irq == IRQ_TIMER0){
        gic_irq_enable(core, PI5_GIC_TIMER_IRQ);
        irq_enable_flag |= irq;
    }
}

inline void irq_clear_core_arch(uint32_t core, uint32_t irq) {
}

inline void irq_clear_arch(uint32_t irq) {
}

void irq_disable_arch(uint32_t irq) {
    (void)irq;
}
