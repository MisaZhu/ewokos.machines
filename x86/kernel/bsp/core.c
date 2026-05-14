#include <kernel/core.h>

#ifdef KERNEL_SMP

void cpu_core_ready(uint32_t core_id) {
	(void)core_id;
}

uint32_t get_cpu_cores(void) {
	return 1;
}

void start_core(uint32_t core_id) {
	(void)core_id;
}

#endif
