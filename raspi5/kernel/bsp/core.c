#include <kernel/core.h>
#include <kernel/system.h>
#include "hw_arch.h"

#ifdef KERNEL_SMP

/* GICv2 helpers from the aarch64 platform layer */
extern void gic_init(void* gicd, void* gicc);
extern void timer_enable_el0_cnt_access(void);

void pi5_dbg_puts(const char* s);

void cpu_core_ready(uint32_t core_id) {
	/* the GIC cpu interface is per-cpu: init it on every core */
	gic_init((void*)(MMIO_BASE + PI5_GICD_OFF),
	         (void*)(MMIO_BASE + PI5_GICC_OFF));
	set_vector_table(&interrupt_table_start);
	/* CNTKCTL_EL1 is per-core: userland timer reads must work on
	 * whichever core the process lands on (bsp/timer.c covers core0) */
	timer_enable_el0_cnt_access();
	ipi_enable(core_id);
	__irq_enable();
}

inline uint32_t get_cpu_cores(void) {
	return 4;
}

#endif
