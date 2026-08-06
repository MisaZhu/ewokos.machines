#include <kernel/hw_info.h>
#include <kernel/kernel.h>
#include <kernel/system.h>
#include <mm/mmu.h>
#include <mm/kalloc.h>
#include <kstring.h>
#include <stdbool.h>
#include <stddef.h>
#include <bcm2712/board.h>
#include <bcm2712/mailbox.h>
#include "hw_arch.h"

#ifdef KERNEL_SMP
#include <kernel/core.h>
#endif

/*
 * Raspberry Pi 5 (BCM2712) hardware info.
 */

uint64_t _core_base_offset = 0;

/*
 * Actual framebuffer base and size reported by the firmware.
 * Updated during sys_info_init_arch() after probing the mailbox.
 * Falls back to the old top-of-RAM assumption if the probe fails.
 */
static ewokos_addr_t _fb_actual_phy = 0;
static ewokos_addr_t _fb_actual_end = 0;  /* (base + size) rounded up to page */
static bool _fb_splits_allocable = false;


/*
 * Map a single 4KB page for device MMIO, walking L1→L2→L3 with 64-bit
 * physical addresses (needed for BCM2712 peripherals above 4GB).
 * Replaces the previous set_block_2mb() which used 2MB block descriptors.
 */
static void set_page_dev(page_dir_entry_t* vm, ewokos_addr_t vaddr, uint64_t phy) {
	uint32_t l1 = PAGE_L1_INDEX(vaddr);
	uint32_t l2 = PAGE_L2_INDEX(vaddr);
	uint32_t l3 = PAGE_L3_INDEX(vaddr);

	/* walk or allocate L2 table */
	if (vm[l1].EntryType == 0) {
		page_table_entry_t* l2_table = (page_table_entry_t*)kalloc4k();
		if (l2_table == NULL)
			return;
		memset(l2_table, 0, PAGE_TABLE_SIZE);
		vm[l1] = (page_dir_entry_t){
			.NSTable = 1,
			.EntryType = TYPE_TABLE,
			.Address = (uint64_t)V2P(l2_table) >> 12,
			.AF = 1,
		};
	}
	page_table_entry_t* l2_table = (page_table_entry_t*)P2V(vm[l1].Address << 12);

	/* walk or allocate L3 table */
	if (l2_table[l2].EntryType == 0) {
		page_table_entry_t* l3_table = (page_table_entry_t*)kalloc4k();
		if (l3_table == NULL)
			return;
		memset(l3_table, 0, PAGE_TABLE_SIZE);
		l2_table[l2] = (page_table_entry_t){
			.NSTable = 1,
			.EntryType = TYPE_TABLE,
			.Address = (uint64_t)V2P(l3_table) >> 12,
			.AF = 1,
		};
	}
	page_table_entry_t* l3_table = (page_table_entry_t*)P2V(l2_table[l2].Address << 12);

	/* populate L3 page descriptor with device memory attributes */
	l3_table[l3] = (page_table_entry_t){
		.NSTable = 1,
		.EntryType = TYPE_PAGE,
		.Address = phy >> 12,
		.AF = 1,
		.SH = STAGE2_SH_OUTER_SHAREABLE,
		.S2AP = 0,
		.MemAttr = MT_DEVICE_NGNRNE,
		.PXN = 1,
		.UXN = 1,
	};

}

void sys_info_init_arch(void) {
	memset(&_sys_info, 0, sizeof(sys_info_t));

	uint32_t board = bcm2712_board();
	uint32_t mem_size = bcm2712_mem_size();

	switch(board) {
		case PI5_2G:   strcpy(_sys_info.machine, "raspberry-pi5-2g");  break;
		case PI5_4G:   strcpy(_sys_info.machine, "raspberry-pi5-4g");  break;
		case PI5_8G:   strcpy(_sys_info.machine, "raspberry-pi5-8g");  break;
		case PI5_16G:  strcpy(_sys_info.machine, "raspberry-pi5-16g"); break;
		case PI5_CM5:  strcpy(_sys_info.machine, "raspberry-cm5");     break;
		case PI5_PI500:strcpy(_sys_info.machine, "raspberry-pi500");   break;
		default:       strcpy(_sys_info.machine, "raspberry-pi5");     break;
	}

	if(board == PI5_UNKNOWN)
		; /* unknown board revision, assuming Pi5 */

	/* RAM size by board model (the mailbox size field is 32bit and
	 * overflows on 4GB+ boards). This OS is 32bit-addressed, so the
	 * usable memory is capped at 2GB anyway. */
	_sys_info.total_phy_mem_size = 2u*GB;
	if(board == PI5_UNKNOWN && mem_size > 64*MB && mem_size <= 2u*GB)
		_sys_info.total_phy_mem_size = mem_size;

	_sys_info.mmio.phy_base = PI5_MMIO_PHY;
	_sys_info.mmio.size = PI5_MMIO_SIZE;
	/*
	 * PI5_MMIO_PHY is a full 64-bit address (0x10_7C000000).
	 * ewokos_addr_t is uint64_t on aarch64, so mmio.phy_base
	 * holds the complete physical address.
	 */
	if(_sys_info.mmio.size > MMIO_MAX_SIZE)
		_sys_info.mmio.size = MMIO_MAX_SIZE;

	_sys_info.total_usable_mem_size = _sys_info.total_phy_mem_size;
	if(_sys_info.total_usable_mem_size > MAX_USABLE_MEM_SIZE)
		_sys_info.total_usable_mem_size = MAX_USABLE_MEM_SIZE;

	strcpy(_sys_info.arch, "aarch64");

	/*
	 * Query the firmware for the actual boot framebuffer location.
	 * On Pi 5 the FB can land anywhere in RAM (often mid-RAM, not
	 * at the very top).  Reserve the exact region so the allocator
	 * doesn't hand it out.
	 */
	{
		uint32_t fb_size = 0;
		ewokos_addr_t fb_base = (ewokos_addr_t)bcm2712_fb_query(&fb_size);

		if (fb_base != 0 && fb_size != 0
				&& fb_base < _sys_info.total_usable_mem_size
				&& fb_base + fb_size <= _sys_info.total_usable_mem_size) {
			_fb_actual_phy = fb_base;
			_fb_actual_end = fb_base
					+ ALIGN_UP((ewokos_addr_t)fb_size, PAGE_SIZE);
			/*
			 * Determine if the FB sits inside the range that
			 * would otherwise be allocable and therefore needs
			 * a "hole" punched out.
			 */
			ewokos_addr_t fb_area_end = fb_base
					+ ALIGN_UP((ewokos_addr_t)fb_size, PAGE_SIZE);
			if (fb_base < (_sys_info.total_phy_mem_size - PI5_FB_SIZE)
					&& fb_area_end > _sys_info.allocable_phy_mem_base) {
				_fb_splits_allocable = true;
			}
		} else {
			/*
			 * Mailbox query failed.  Reserve a fallback 8 MB
			 * at the top of allocable RAM so that kalloc never
			 * hands out pages that might overlap the display
			 * framebuffer.  The top PI5_FB_SIZE is already
			 * excluded via allocable_phy_mem_top below, but
			 * we also need _fb_actual_phy non-zero so that
			 * check_mem_map_arch() continues to guard SYS_MEM_MAP
			 * requests.
			 */
			_fb_actual_phy = _sys_info.total_phy_mem_size
					- PI5_FB_SIZE;
			_fb_actual_end = _sys_info.total_phy_mem_size;
			_fb_splits_allocable = true;
		}
	}

	/* reserve the top of RAM for the firmware framebuffer */
	_sys_info.allocable_phy_mem_top = _sys_info.phy_offset +
			_sys_info.total_usable_mem_size - PI5_FB_SIZE;

#ifdef KERNEL_SMP
	_sys_info.cores = 1;//get_cpu_cores();
#else
	_sys_info.cores = 1;
#endif

	_sys_info.vector_base = (ewokos_addr_t)&interrupt_table_start;
}

void arch_vm(page_dir_entry_t* vm) {
	/*
	 * Map MMIO windows using 4KB pages (TYPE_PAGE at L3), replacing the
	 * previous 2MB block mappings (TYPE_BLOCK at L2).  set_page_dev()
	 * carries full 64-bit physical addresses for peripheral windows
	 * above 4GB (EMMC, RP1).
	 */


	/* Main MMIO window: 64 MB at 0x10_7C000000 */
	for (uint32_t i = 0; i < PI5_MMIO_SIZE / PAGE_SIZE; i++) {
		set_page_dev(vm, MMIO_BASE + i * PAGE_SIZE,
			     PI5_MMIO_PHY + i * PAGE_SIZE);
	}

	/* SD host controller window: 2 MB at 0x10_00E00000 */
	for (uint32_t i = 0; i < (2 * MB) / PAGE_SIZE; i++) {
		set_page_dev(vm, MMIO_BASE + PI5_EMMC_WIN_OFF + i * PAGE_SIZE,
			     PI5_EMMC_PHY_WIN + i * PAGE_SIZE);
	}

	/* RP1 southbridge window: 32 MB at 0x1F_00000000 */
	for (uint32_t i = 0; i < PI5_RP1_SIZE / PAGE_SIZE; i++) {
		set_page_dev(vm, MMIO_BASE + PI5_RP1_WIN_OFF + i * PAGE_SIZE,
			     PI5_RP1_PHY + i * PAGE_SIZE);
	}
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
	ewokos_addr_t start = P2V(_sys_info.allocable_phy_mem_base);

	if (_fb_splits_allocable) {
		/*
		 * The firmware's framebuffer sits inside the otherwise-
		 * allocable range.  Punch a hole: one region below the FB,
		 * another above it (up to the standard top-of-RAM reserve).
		 */
		kalloc_append(start, P2V(_fb_actual_phy));
		if (_fb_actual_end < _sys_info.allocable_phy_mem_top)
			kalloc_append(P2V(_fb_actual_end),
				      P2V(_sys_info.allocable_phy_mem_top));
	} else {
		kalloc_append(start, P2V(_sys_info.allocable_phy_mem_top));
	}
}

int32_t check_mem_map_arch(ewokos_addr_t phy_base, uint32_t size) {
	/*
	 * Framebuffer: accept the top-of-RAM recall window (always
	 * reserved) plus the firmware's actual boot FB region when it
	 * lands elsewhere in RAM.
	 */
	if (phy_base >= _sys_info.total_phy_mem_size - PI5_FB_SIZE)
		return 0;
	if (_fb_actual_phy != 0
			&& phy_base >= _fb_actual_phy
			&& phy_base + size <= _fb_actual_end)
		return 0;

	/* main MMIO window: 64 MB at 0x10_7C000000 */
	if (phy_base >= PI5_MMIO_PHY &&
	    phy_base + size <= PI5_MMIO_PHY + PI5_MMIO_SIZE)
		return 0;

	/* SD host controller (EMMC) window: 2 MB at 0x10_00E00000 */
	if (phy_base >= PI5_EMMC_PHY_WIN &&
	    phy_base + size <= PI5_EMMC_PHY_WIN + 2 * MB)
		return 0;

	/* RP1 southbridge window: 32 MB at 0x1F_00000000 */
	if (phy_base >= PI5_RP1_PHY &&
	    phy_base + size <= PI5_RP1_PHY + PI5_RP1_SIZE)
		return 0;

	return -1;
}
