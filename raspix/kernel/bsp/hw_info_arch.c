#include <kernel/hw_info.h>
#include <kernel/kernel.h>
#include <kernel/system.h>
#include <mm/mmu.h>
#include <kstring.h>
#include <stdbool.h>
#include <bcm283x/board.h>
#include "hw_arch.h"

#ifdef KERNEL_SMP
#include <kernel/core.h>
#endif

ewokos_addr_t _core_base_offset = 0;
uint32_t _uart_type = UART_MINI;
uint32_t _pi4 = 0;
	
/* phy memory reservation for framebuffer and mmio*/
#define PHY_LOW_RESV_SIZE (76*MB)
#define PHY_LOW_RESV_BASE (1u*GB - PHY_LOW_RESV_SIZE)

#define PHY_HIGH_RESV_SIZE (64*MB)
#define PHY_HIGH_RESV_BASE (4ull*GB - PHY_HIGH_RESV_SIZE)

void sys_info_init_arch(void) {
	memset(&_sys_info, 0, sizeof(sys_info_t));
	uint32_t pix_revision = bcm283x_board();
	_sys_info.total_phy_mem_size = 512*MB;
	_core_base_offset =  0x01000000;
	_sys_info.mmio.phy_base = 0x3f000000;
	_pi4 = 0;
	_uart_type = UART_MINI;

	if(pix_revision == PI_4B_1G) {
		strcpy(_sys_info.machine, "raspberry-pi4b-1g");
		_sys_info.total_phy_mem_size = 1u*GB;
		_sys_info.mmio.phy_base = 0xfe000000;
		_core_base_offset =  0x01800000;
		_pi4 = 1;
	}
	else if(pix_revision == PI_4B_2G) {
		strcpy(_sys_info.machine, "raspberry-pi4b-2G");
		_sys_info.total_phy_mem_size = 2u*GB;
		_sys_info.mmio.phy_base = 0xfe000000;
		_core_base_offset =  0x01800000;
		_pi4 = 1;
	}
	else if(pix_revision == PI_4B_4G) {
		strcpy(_sys_info.machine, "raspberry-pi4b-4G");
		_sys_info.total_phy_mem_size = 4ull*GB;
		_sys_info.mmio.phy_base = 0xfe000000;
		_core_base_offset =  0x01800000;
		_pi4 = 1;
	}
	else if(pix_revision == PI_4B_8G) {
		strcpy(_sys_info.machine, "raspberry-pi4b-8G");
		_sys_info.total_phy_mem_size = 8ull*GB; //max for 32bits os
		_sys_info.mmio.phy_base = 0xfe000000;
		_core_base_offset =  0x01800000;
		_pi4 = 1;
	}
	else if(pix_revision == PI_CM4_1G) {
		strcpy(_sys_info.machine, "raspberry-cm4-1g");
		_sys_info.total_phy_mem_size = 1u*GB;
		_sys_info.mmio.phy_base = 0xfe000000;
		_core_base_offset =  0x01800000;
		_pi4 = 1;
	}
	else if(pix_revision == PI_CM4_2G) {
		strcpy(_sys_info.machine, "raspberry-cm4-2G");
		_sys_info.total_phy_mem_size = 2u*GB;
		_sys_info.mmio.phy_base = 0xfe000000;
		_core_base_offset =  0x01800000;
		_pi4 = 1;
	}
	else if(pix_revision == PI_CM4_4G) {
		strcpy(_sys_info.machine, "raspberry-cm4-4G");
		_sys_info.total_phy_mem_size = 4ull*GB;
		_sys_info.mmio.phy_base = 0xfe000000;
		_core_base_offset =  0x01800000;
		_pi4 = 1;
	}
	else if(pix_revision == PI_CM4_8G) {
		strcpy(_sys_info.machine, "raspberry-cm4-8G");
		_sys_info.total_phy_mem_size = 8ull*GB;
		_sys_info.mmio.phy_base = 0xfe000000;
		_core_base_offset =  0x01800000;
		_pi4 = 1;
	}
	else if(pix_revision == PI_CM3_1G) {
		strcpy(_sys_info.machine, "raspberry-cm3-1G");
		_sys_info.total_phy_mem_size = 1u*GB;
		_sys_info.mmio.phy_base = 0x3f000000;
	}
	else if(pix_revision == PI_2B) {
		strcpy(_sys_info.machine, "raspberry-pi2b");
		_sys_info.total_phy_mem_size = 1u*GB;
		_sys_info.mmio.phy_base = 0x3f000000;
		_uart_type = UART_PL011;
	}
	else if(pix_revision == PI_3A) {
		strcpy(_sys_info.machine, "raspberry-pi3a");
		_sys_info.total_phy_mem_size = 512*MB;
		_sys_info.mmio.phy_base = 0x3f000000;
	}
	else if(pix_revision == PI_0_2W) {
		strcpy(_sys_info.machine, "raspberry-pi2w");
		_sys_info.total_phy_mem_size = 512*MB;
		_sys_info.mmio.phy_base = 0x3f000000;
	}
	else if(pix_revision == PI_3B) {
		strcpy(_sys_info.machine, "raspberry-pi3b");
		_sys_info.total_phy_mem_size = 1u*GB;
		_sys_info.mmio.phy_base = 0x3f000000;
	}
	else if(pix_revision == PI_5_2G) {
		strcpy(_sys_info.machine, "raspberry-pi5-2g");
		_sys_info.total_phy_mem_size = 2u*GB;
		_sys_info.mmio.phy_base = 0x7c000000;
		_uart_type = UART_PL011;
	}
	else if(pix_revision == PI_5_4G) {
		strcpy(_sys_info.machine, "raspberry-pi5-4g");
		_sys_info.total_phy_mem_size = 4ull*GB;
		_sys_info.mmio.phy_base = 0x7c000000;
		_uart_type = UART_PL011;
	}
	else if(pix_revision == PI_5_8G) {
		strcpy(_sys_info.machine, "raspberry-pi5-8g");
		_sys_info.total_phy_mem_size = 8ull*GB;
		_sys_info.mmio.phy_base = 0x7c000000;
		_uart_type = UART_PL011;
	}
	else if(pix_revision == PI_5_16G) {
		strcpy(_sys_info.machine, "raspberry-pi5-16g");
		_sys_info.total_phy_mem_size = 16ull*GB;
		_sys_info.mmio.phy_base = 0x7c000000;
		_uart_type = UART_PL011;
	}

	_sys_info.total_usable_mem_size = _sys_info.total_phy_mem_size;
	if(_sys_info.total_usable_mem_size > (ewokos_addr_t)MAX_USABLE_MEM_SIZE)
		_sys_info.total_usable_mem_size = MAX_USABLE_MEM_SIZE;

#if __aarch64__
	strcpy(_sys_info.arch, "aarch64");
#elif __arm__
	strcpy(_sys_info.arch, "armv7");
#endif

	_sys_info.mmio.size = 32*MB;
	_sys_info.allocable_phy_mem_top = _sys_info.phy_offset +
		_sys_info.total_usable_mem_size;

	if(_sys_info.total_usable_mem_size > 1*GB)
		 _sys_info.allocable_phy_mem_top -= PHY_HIGH_RESV_SIZE;
	else
		 _sys_info.allocable_phy_mem_top -= PHY_LOW_RESV_SIZE;

#ifdef KERNEL_SMP
	_sys_info.cores = get_cpu_cores();
#else
	_sys_info.cores = 1;
#endif

#if __aarch64__
	_sys_info.vector_base = (ewokos_addr_t)&interrupt_table_start;
#endif 
}

void arch_vm(page_dir_entry_t* vm) {
	ewokos_addr_t vbase = _sys_info.mmio.v_base + _core_base_offset;
	ewokos_addr_t pbase = _sys_info.mmio.phy_base + _core_base_offset;
	map_page(vm, vbase, pbase, AP_RW_RW, PTE_ATTR_DEV);
	//map_page(vm, pbase, pbase, AP_RW_D, PTE_ATTR_DEV);
}


#ifdef KERNEL_SMP
extern char __entry[];
void start_core(uint32_t core_id) {
    if(core_id >= _sys_info.cores)
        return;
#if __arm__
    ewokos_addr_t core_start_addr = (core_id * 0x10 + 0x8c) + 
       _sys_info.mmio.v_base + 
       _core_base_offset;

    put32(core_start_addr, (ewokos_addr_t)__entry);
#elif __aarch64__
	ewokos_addr_t core_start_addr = P2V(0xE0) + (core_id - 1) * 8;
	*(volatile uint32_t*)core_start_addr = (ewokos_addr_t)__entry;
	flush_dcache();
#endif
    __asm__("sev");
}
#endif

void kalloc_arch(void) {
	/*
	 * Add allocatable RAM to kalloc while skipping mmio reserved
	 * holes in both the low memory window and the upper memory range.
	 */
	if(_sys_info.allocable_phy_mem_top > 1*GB) {
		kalloc_append(P2V(_sys_info.allocable_phy_mem_base), P2V(PHY_LOW_RESV_BASE));
		if(_sys_info.allocable_phy_mem_top > PHY_HIGH_RESV_BASE) {
			kalloc_append(P2V(1*GB), P2V(PHY_HIGH_RESV_BASE));
			if(_sys_info.allocable_phy_mem_top > (PHY_HIGH_RESV_BASE+PHY_HIGH_RESV_SIZE))
				kalloc_append(P2V(PHY_HIGH_RESV_BASE+PHY_HIGH_RESV_SIZE), P2V(_sys_info.allocable_phy_mem_top));
		}
		else
			kalloc_append(P2V(1*GB), P2V(_sys_info.allocable_phy_mem_top));
	}
	else
		kalloc_append(P2V(_sys_info.allocable_phy_mem_base), P2V(_sys_info.allocable_phy_mem_top));
}

int32_t  check_mem_map_arch(ewokos_addr_t phy_base, uint32_t size) {
	if(phy_base >= PHY_LOW_RESV_BASE && size <= PHY_LOW_RESV_SIZE)
		return 0;
	if(phy_base >= PHY_HIGH_RESV_BASE && size <= PHY_HIGH_RESV_SIZE)
		return 0;

	if(_sys_info.total_phy_mem_size < 1*GB && 
			phy_base >= _sys_info.total_phy_mem_size - PHY_LOW_RESV_SIZE)
		return 0;
	return -1;
}

static inline int32_t range_in_ram_window(ewokos_addr_t phy_base,
		ewokos_addr_t map_end,
		ewokos_addr_t ram_base,
		ewokos_addr_t ram_end) {
	if(phy_base < ram_base)
		return 0;
	if(map_end < phy_base)
		return 0;
	if(map_end > ram_end)
		return 0;
	return 1;
}

typedef struct mem_window {
	ewokos_addr_t base;
	ewokos_addr_t end;
} mem_window_t;

int32_t mem_map_is_normal_ram_arch(ewokos_addr_t phy_base, uint32_t size) {
	ewokos_addr_t map_end = phy_base + size;
	mem_window_t windows[3];
	uint32_t window_num = 0;
	uint32_t i;

	if(map_end < phy_base)
		return 0;

	if(_sys_info.total_usable_mem_size <= 1*GB) {
		windows[window_num].base = _sys_info.allocable_phy_mem_base;
		windows[window_num].end = _sys_info.allocable_phy_mem_top;
		window_num++;
	}
	else {
		windows[window_num].base = _sys_info.allocable_phy_mem_base;
		windows[window_num].end = PHY_LOW_RESV_BASE;
		window_num++;

		if(_sys_info.total_usable_mem_size <= 4ull*GB) {
			windows[window_num].base = 1*GB;
			windows[window_num].end = _sys_info.allocable_phy_mem_top;
			window_num++;
		}
		else {
			windows[window_num].base = 1*GB;
			windows[window_num].end = PHY_HIGH_RESV_BASE;
			window_num++;

			windows[window_num].base = PHY_HIGH_RESV_BASE + PHY_HIGH_RESV_SIZE;
			windows[window_num].end = _sys_info.allocable_phy_mem_top;
			window_num++;
		}
	}

	for(i = 0; i < window_num; i++) {
		if(range_in_ram_window(phy_base, map_end,
					windows[i].base,
					windows[i].end)) {
			return 1;
		}
	}

	return 0;
}
