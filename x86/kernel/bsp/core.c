#include <kernel/core.h>
#include <kernel/kernel.h>
#include <kernel/system.h>
#include <kstring.h>
#include "x86_machine_smp.h"

#ifdef KERNEL_SMP

extern uint32_t __cpu_cores(void);
extern uint32_t __core_id(void);
extern void _slave_kernel_entry_c(void);
extern void x86_runtime_init(void);
extern uint64_t boot_pml4;

static uint8_t _ap_boot_stacks[CPU_MAX_CORES][PAGE_SIZE] __attribute__((aligned(16)));
static uint32_t _x86_core_to_apic[CPU_MAX_CORES];
static uint8_t _x86_apic_to_core[256];
static uint8_t _x86_smp_map_inited = 0;
static uint32_t _x86_detected_cores = 0;

static inline uint32_t x86_core_count(void) {
	if (_x86_detected_cores != 0) {
		return _x86_detected_cores;
	}

	uint32_t cores = __cpu_cores();

	if (cores == 0) {
		cores = 1;
	}
	if (cores > CPU_MAX_CORES) {
		cores = CPU_MAX_CORES;
	}
	return cores;
}

void x86_smp_mapping_reset(void) {
	memset(_x86_apic_to_core, 0xFF, sizeof(_x86_apic_to_core));
	memset(_x86_core_to_apic, 0, sizeof(_x86_core_to_apic));
	_x86_detected_cores = 0;
	_x86_smp_map_inited = 0;
}

void x86_smp_set_apic_id(uint32_t core_id, uint32_t apic_id) {
	if (core_id >= CPU_MAX_CORES) {
		return;
	}

	_x86_core_to_apic[core_id] = apic_id & 0xFFu;
	_x86_apic_to_core[apic_id & 0xFFu] = (uint8_t)core_id;
	if ((core_id + 1) > _x86_detected_cores) {
		_x86_detected_cores = core_id + 1;
	}
}

uint32_t x86_smp_detected_core_count(void) {
	return _x86_detected_cores;
}

void x86_smp_mapping_init(void) {
	uint32_t bsp_apic_id;
	uint32_t cores;
	uint32_t next_apic;

	if (_x86_smp_map_inited != 0) {
		return;
	}

	if (_x86_detected_cores != 0) {
		_x86_smp_map_inited = 1;
		return;
	}

	x86_smp_mapping_reset();

	cores = x86_core_count();
	bsp_apic_id = __core_id() & 0xFFu;
	x86_smp_set_apic_id(0, bsp_apic_id);

	next_apic = 0;
	for (uint32_t core_id = 1; core_id < cores; core_id++) {
		while (next_apic == bsp_apic_id && next_apic < 255) {
			next_apic++;
		}
		x86_smp_set_apic_id(core_id, next_apic);
		next_apic++;
	}
	_x86_smp_map_inited = 1;
}

uint32_t x86_apic_id_to_core_id(uint32_t apic_id) {
	uint8_t core_id;

	x86_smp_mapping_init();
	core_id = _x86_apic_to_core[apic_id & 0xFFu];
	if (core_id == 0xFFu) {
		return apic_id;
	}
	return core_id;
}

uint32_t x86_core_id_to_apic_id(uint32_t core_id) {
	x86_smp_mapping_init();
	if (core_id >= CPU_MAX_CORES) {
		return core_id;
	}
	return _x86_core_to_apic[core_id];
}

static void __attribute__((unused)) x86_prepare_ap_trampoline(uint32_t core_id) {
	uint8_t *trampoline = (uint8_t *)X86_AP_TRAMPOLINE_VADDR;
	uintptr_t offset_boot_pml4;
	uintptr_t offset_kernel_vm;
	uintptr_t offset_stack;
	uintptr_t offset_kernel_entry;
	uintptr_t offset_core_id;
	uint32_t size = (uint32_t)(x86_ap_trampoline_end - x86_ap_trampoline_start);
	uint64_t stack_top;

	memcpy(trampoline, x86_ap_trampoline_start, size);

	offset_boot_pml4 = (uintptr_t)(x86_ap_trampoline_boot_pml4 - x86_ap_trampoline_start);
	offset_kernel_vm = (uintptr_t)(x86_ap_trampoline_kernel_vm - x86_ap_trampoline_start);
	offset_stack = (uintptr_t)(x86_ap_trampoline_stack - x86_ap_trampoline_start);
	offset_kernel_entry = (uintptr_t)(x86_ap_trampoline_kernel_entry - x86_ap_trampoline_start);
	offset_core_id = (uintptr_t)(x86_ap_trampoline_core_id - x86_ap_trampoline_start);
	stack_top = (uint64_t)(uintptr_t)&_ap_boot_stacks[core_id][PAGE_SIZE - 16];

	put64((uintptr_t)trampoline + offset_boot_pml4, (uint64_t)(uintptr_t)&boot_pml4);
	put64((uintptr_t)trampoline + offset_kernel_vm,
		(uint64_t)V2P((ewokos_addr_t)_kernel_info.kernel_vm));
	put64((uintptr_t)trampoline + offset_stack, stack_top);
	put64((uintptr_t)trampoline + offset_kernel_entry, (uint64_t)(uintptr_t)&_slave_kernel_entry_c);
	put32((uintptr_t)trampoline + offset_core_id, core_id);
	flush_dcache();
}

void cpu_core_ready(uint32_t core_id) {
	x86_smp_set_apic_id(core_id, __core_id() & 0xFFu);
	x86_runtime_init();
	x86_irq_percpu_init();
	__irq_enable();
}

uint32_t get_cpu_cores(void) {
	x86_smp_mapping_init();
	return x86_core_count();
}

void start_core(uint32_t core_id) {
	uint32_t apic_id;

	if (core_id == 0 || core_id >= x86_core_count()) {
		return;
	}
	apic_id = x86_core_id_to_apic_id(core_id);
	x86_prepare_ap_trampoline(core_id);
	x86_start_ap(apic_id);
}

#endif
