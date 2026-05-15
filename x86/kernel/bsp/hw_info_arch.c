#include <kernel/hw_info.h>
#include <kernel/kernel.h>
#include <kernel/core.h>
#include <kernel/system.h>
#include <mm/mmu.h>
#include <mm/kalloc.h>
#include <kprintf.h>
#include <kstring.h>
#include <stdbool.h>
#include <stddef.h>
#include "x86_machine_smp.h"

extern uint32_t interrupt_table_start;

ewokos_addr_t _core_base_offset = 0;

typedef struct __attribute__((packed)) {
	char signature[4];
	uint32_t config_table;
	uint8_t length;
	uint8_t spec_rev;
	uint8_t checksum;
	uint8_t feature1;
	uint8_t feature2;
	uint8_t feature3[3];
} x86_mp_floating_t;

typedef struct __attribute__((packed)) {
	char signature[4];
	uint16_t base_table_length;
	uint8_t spec_rev;
	uint8_t checksum;
	char oem_id[8];
	char product_id[12];
	uint32_t oem_table;
	uint16_t oem_table_size;
	uint16_t entry_count;
	uint32_t lapic_addr;
	uint16_t extended_table_length;
	uint8_t extended_table_checksum;
	uint8_t reserved;
} x86_mp_config_t;

typedef struct __attribute__((packed)) {
	uint8_t type;
	uint8_t apic_id;
	uint8_t apic_version;
	uint8_t cpu_flags;
	uint32_t cpu_signature;
	uint32_t feature_flags;
	uint32_t reserved[2];
} x86_mp_cpu_entry_t;

#define X86_MP_CPU_ENTRY           0
#define X86_MP_CPU_ENABLED         0x01
#define X86_MP_CPU_BSP             0x02

static uint8_t x86_sum_bytes(const void *ptr, uint32_t size) {
	const uint8_t *p = (const uint8_t *)ptr;
	uint8_t sum = 0;

	for (uint32_t i = 0; i < size; i++) {
		sum = (uint8_t)(sum + p[i]);
	}
	return sum;
}

static uint16_t x86_bda_read16(uintptr_t addr) {
	uint16_t value;

	__asm__ volatile("movw (%1), %0" : "=r"(value) : "r"(addr) : "memory");
	return value;
}

static const x86_mp_floating_t* x86_find_mp_floating_in_range(uintptr_t start, uintptr_t end) {
	for (uintptr_t addr = start; (addr + sizeof(x86_mp_floating_t)) <= end; addr += 16) {
		const x86_mp_floating_t *mp = (const x86_mp_floating_t *)addr;

		if (memcmp((void *)mp->signature, (void *)"_MP_", 4) != 0) {
			continue;
		}
		if (mp->length == 0) {
			continue;
		}
		if (x86_sum_bytes(mp, (uint32_t)mp->length * 16) != 0) {
			continue;
		}
		return mp;
	}
	return NULL;
}

static const x86_mp_floating_t* x86_find_mp_floating(void) {
	uintptr_t ebda_addr = ((uintptr_t)x86_bda_read16(0x40E)) << 4;
	uintptr_t base_kb = (uintptr_t)x86_bda_read16(0x413);
	const x86_mp_floating_t *mp;

	if (ebda_addr != 0) {
		mp = x86_find_mp_floating_in_range(ebda_addr, ebda_addr + 1024);
		if (mp != NULL) {
			return mp;
		}
	}

	if (base_kb >= 1) {
		uintptr_t top_of_base = base_kb * 1024;
		mp = x86_find_mp_floating_in_range(top_of_base - 1024, top_of_base);
		if (mp != NULL) {
			return mp;
		}
	}

	return x86_find_mp_floating_in_range(0xF0000, 0x100000);
}

static void x86_detect_mp_topology(void) {
	const x86_mp_floating_t *mp;
	const x86_mp_config_t *cfg;
	const uint8_t *entry;
	uint32_t count = 0;
	uint8_t bsp_apic = 0xFF;
	uint8_t apics[CPU_MAX_CORES];

	memset(apics, 0xFF, sizeof(apics));
	x86_smp_mapping_reset();
	mp = x86_find_mp_floating();
	if (mp == NULL || mp->config_table == 0) {
		return;
	}
	if (mp->config_table < 0x400 || mp->config_table >= (1024 * MB)) {
		return;
	}

	cfg = (const x86_mp_config_t *)(uintptr_t)mp->config_table;
	if (memcmp((void *)cfg->signature, (void *)"PCMP", 4) != 0) {
		return;
	}
	if (cfg->base_table_length < sizeof(x86_mp_config_t) ||
			cfg->base_table_length > 4096) {
		return;
	}
	if (x86_sum_bytes(cfg, cfg->base_table_length) != 0) {
		return;
	}

	entry = (const uint8_t *)(cfg + 1);
	for (uint32_t i = 0; i < cfg->entry_count && count < CPU_MAX_CORES; i++) {
		switch (entry[0]) {
		case X86_MP_CPU_ENTRY: {
			const x86_mp_cpu_entry_t *cpu = (const x86_mp_cpu_entry_t *)entry;
			if ((cpu->cpu_flags & X86_MP_CPU_ENABLED) != 0) {
				if ((cpu->cpu_flags & X86_MP_CPU_BSP) != 0) {
					bsp_apic = cpu->apic_id;
				}
				apics[count++] = cpu->apic_id;
			}
			entry += sizeof(x86_mp_cpu_entry_t);
			break;
		}
		case 1:
		case 2:
		case 3:
		case 4:
			entry += 8;
			break;
		default:
			return;
		}
	}

	if (count == 0) {
		return;
	}

	if (bsp_apic == 0xFF) {
		bsp_apic = apics[0];
	}

	x86_smp_set_apic_id(0, bsp_apic);
	for (uint32_t i = 0, core_id = 1; i < count && core_id < CPU_MAX_CORES; i++) {
		if (apics[i] == bsp_apic) {
			continue;
		}
		x86_smp_set_apic_id(core_id++, apics[i]);
	}
}

void sys_info_init_arch(void) {
	memset(&_sys_info, 0, sizeof(sys_info_t));
	_sys_info.phy_offset = 0x00100000;
	_sys_info.vector_base = (ewokos_addr_t)&interrupt_table_start;
	_sys_info.total_phy_mem_size = 512 * MB;
	_sys_info.total_usable_mem_size = _sys_info.total_phy_mem_size - _sys_info.phy_offset;
	if (_sys_info.total_usable_mem_size > (uint32_t)(MAX_USABLE_MEM_SIZE - _sys_info.phy_offset)) {
		_sys_info.total_usable_mem_size = (uint32_t)(MAX_USABLE_MEM_SIZE - _sys_info.phy_offset);
	}
	_sys_info.mmio.phy_base = 0xFD000000;
	_sys_info.mmio.size = 0x02000000;
	_sys_info.sys_dma.size = 16 * MB;
	_sys_info.machine[0] = 'x';
	_sys_info.machine[1] = '8';
	_sys_info.machine[2] = '6';
	_sys_info.machine[3] = '\0';
	_sys_info.arch[0] = 'x';
	_sys_info.arch[1] = '8';
	_sys_info.arch[2] = '6';
	_sys_info.arch[3] = '_';
	_sys_info.arch[4] = '6';
	_sys_info.arch[5] = '4';
	_sys_info.arch[6] = '\0';
	x86_detect_mp_topology();
	_sys_info.cores = get_cpu_cores();
	_sys_info.allocable_phy_mem_top = _sys_info.phy_offset + _sys_info.total_usable_mem_size;
}

void arch_vm(page_dir_entry_t* vm) {
	map_pages_size(vm, X86_AP_TRAMPOLINE_VADDR, X86_AP_TRAMPOLINE_PADDR,
			PAGE_SIZE, AP_RW_D, PTE_ATTR_WRBACK);
}

void kalloc_arch(void) {
	ewokos_addr_t base = P2V(_sys_info.allocable_phy_mem_base);
	page_list_t* head;
	uint32_t pages = kalloc_append(P2V(_sys_info.allocable_phy_mem_base), P2V(_sys_info.allocable_phy_mem_top));
	head = (page_list_t*)(P2V(_sys_info.allocable_phy_mem_top) - PAGE_SIZE);
	printf("kalloc_arch: base=%x top=%x map=%x/%x/%x head=%x next=%x next2=%x pages=%d free=%d\n",
			base,
			P2V(_sys_info.allocable_phy_mem_top),
			resolve_phy_address(_kernel_info.kernel_vm, base),
			resolve_phy_address(_kernel_info.kernel_vm, base + PAGE_SIZE),
			resolve_phy_address(_kernel_info.kernel_vm, base + PAGE_SIZE * 2),
			(ewokos_addr_t)head,
			head != NULL ? (ewokos_addr_t)head->next : 0,
			(head != NULL && head->next != NULL) ? (ewokos_addr_t)head->next->next : 0,
			pages,
			get_free_mem_size());
}

int32_t check_mem_map_arch(ewokos_addr_t phy_base, uint32_t size) {
	ewokos_addr_t mmio_end;
	ewokos_addr_t map_end;
	if (_sys_info.mmio.size == 0) {
		(void)phy_base;
		(void)size;
		return -1;
	}
	mmio_end = _sys_info.mmio.phy_base + _sys_info.mmio.size;
	map_end = phy_base + size;
	if (phy_base >= _sys_info.mmio.phy_base && map_end >= phy_base && map_end <= mmio_end) {
		return 0;
	}
	return -1;
}
