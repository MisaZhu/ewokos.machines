#include <kernel/hw_info.h>
#include <kernel/kernel.h>
#include <kernel/system.h>
#include <mm/mmu.h>
#include <mm/kalloc.h>
#include <kprintf.h>
#include <kstring.h>
#include <stdbool.h>
#include <stddef.h>

extern uint32_t interrupt_table_start;

ewokos_addr_t _core_base_offset = 0;

void sys_info_init_arch(void) {
	memset(&_sys_info, 0, sizeof(sys_info_t));
	_sys_info.phy_offset = 0x00100000;
	_sys_info.vector_base = (ewokos_addr_t)&interrupt_table_start;
	_sys_info.total_phy_mem_size = 512 * MB;
	_sys_info.total_usable_mem_size = _sys_info.total_phy_mem_size - _sys_info.phy_offset;
	if (_sys_info.total_usable_mem_size > (uint32_t)(MAX_USABLE_MEM_SIZE - _sys_info.phy_offset)) {
		_sys_info.total_usable_mem_size = (uint32_t)(MAX_USABLE_MEM_SIZE - _sys_info.phy_offset);
	}
	_sys_info.mmio.phy_base = 0xE0000000;
	_sys_info.mmio.size = 0x20000000;
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
	_sys_info.cores = 1;
	_sys_info.allocable_phy_mem_top = _sys_info.phy_offset + _sys_info.total_usable_mem_size;
}

void arch_vm(page_dir_entry_t* vm) {
	(void)vm;
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
	if (_sys_info.mmio.size == 0) {
		(void)phy_base;
		(void)size;
		return -1;
	}
	if (phy_base >= _sys_info.mmio.phy_base && size <= _sys_info.mmio.size) {
		return 0;
	}
	return -1;
}
