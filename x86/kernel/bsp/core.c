#include <kernel/core.h>
#include <kernel/kernel.h>
#include <kernel/system.h>
#include <kstring.h>
#include "x86_machine_smp.h"

#ifdef KERNEL_SMP

extern uint32_t __cpu_cores(void);
extern void _slave_kernel_entry_c(void);
extern uint64_t boot_pml4;

static uint8_t _ap_boot_stacks[CPU_MAX_CORES][PAGE_SIZE] __attribute__((aligned(16)));

static inline uint32_t x86_core_count(void) {
	uint32_t cores = __cpu_cores();

	if (cores == 0) {
		cores = 1;
	}
	if (cores > CPU_MAX_CORES) {
		cores = CPU_MAX_CORES;
	}
	return cores;
}

static void x86_prepare_ap_trampoline(uint32_t core_id) {
	uint8_t *trampoline = (uint8_t *)X86_AP_TRAMPOLINE_VADDR;
	uintptr_t offset_boot_pml4;
	uintptr_t offset_kernel_vm;
	uintptr_t offset_stack;
	uint32_t size = (uint32_t)(x86_ap_trampoline_end - x86_ap_trampoline_start);
	uint64_t stack_top;

	memcpy(trampoline, x86_ap_trampoline_start, size);

	offset_boot_pml4 = (uintptr_t)(x86_ap_trampoline_boot_pml4 - x86_ap_trampoline_start);
	offset_kernel_vm = (uintptr_t)(x86_ap_trampoline_kernel_vm - x86_ap_trampoline_start);
	offset_stack = (uintptr_t)(x86_ap_trampoline_stack - x86_ap_trampoline_start);
	stack_top = (uint64_t)(uintptr_t)&_ap_boot_stacks[core_id][PAGE_SIZE - 16];

	put64((uintptr_t)trampoline + offset_boot_pml4, (uint64_t)V2P((ewokos_addr_t)&boot_pml4));
	put64((uintptr_t)trampoline + offset_kernel_vm,
		(uint64_t)V2P((ewokos_addr_t)_kernel_info.kernel_vm));
	put64((uintptr_t)trampoline + offset_stack, stack_top);
	flush_dcache();
}

void cpu_core_ready(uint32_t core_id) {
	(void)core_id;
	x86_irq_percpu_init();
	__irq_enable();
}

uint32_t get_cpu_cores(void) {
	return x86_core_count();
}

void start_core(uint32_t core_id) {
	if (core_id == 0 || core_id >= x86_core_count()) {
		return;
	}
	x86_prepare_ap_trampoline(core_id);
	x86_start_ap(core_id);
}

#endif
