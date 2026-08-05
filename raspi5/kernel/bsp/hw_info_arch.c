#include <kernel/hw_info.h>
#include <kernel/kernel.h>
#include <kernel/system.h>
#include <mm/mmu.h>
#include <mm/kalloc.h>
#include <kstring.h>
#include <stdbool.h>
#include <stddef.h>
#include <bcm2712/board.h>
#include "hw_arch.h"

#ifdef KERNEL_SMP
#include <kernel/core.h>
#endif

/*
 * Raspberry Pi 5 (BCM2712) hardware info.
 */

uint64_t _core_base_offset = 0;

void pi5_dbg_puts(const char* s);
void pi5_dbg_putc(char c);

/*
 * Map a single 4KB page for device MMIO, walking L1→L2→L3 with 64-bit
 * physical addresses (needed for BCM2712 peripherals above 4GB).
 * Replaces the previous set_block_2mb() which used 2MB block descriptors.
 */
static void set_page_dev(page_dir_entry_t* vm, uint32_t vaddr, uint64_t phy) {
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
	 * PI5_MMIO_PHY may exceed 32 bits (0x10_7C000000 > 4GB).
	 * ewokos_addr_t is uint32_t, so mmio.phy_base truncates to 0x7C000000.
	 * The boot page tables use the full uint64_t address, so early UART
	 * debug output works. The final kernel VM also uses mmio.phy_base,
	 * so after the kernel VM switch peripherals are only reachable if
	 * the BCM2712 hardware enables the 32-bit alias at 0x7C000000.
	 */
	if(_sys_info.mmio.size > MMIO_MAX_SIZE)
		_sys_info.mmio.size = MMIO_MAX_SIZE;

	_sys_info.total_usable_mem_size = _sys_info.total_phy_mem_size;
	if(_sys_info.total_usable_mem_size > (uint32_t)MAX_USABLE_MEM_SIZE)
		_sys_info.total_usable_mem_size = MAX_USABLE_MEM_SIZE;

	strcpy(_sys_info.arch, "aarch64");

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
	/* firmware spin table: secondary cores poll these slots */
	uint64_t core_start_addr = 0x800000E0 + (core_id - 1) * 8;
	*(volatile uint32_t*)core_start_addr = (uint32_t)__entry;
	flush_dcache();
	__asm__("sev");
}
#endif

void kalloc_arch(void) {
	kalloc_append(P2V(_sys_info.allocable_phy_mem_base), P2V(_sys_info.allocable_phy_mem_top));
}

int32_t check_mem_map_arch(ewokos_addr_t phy_base, uint32_t size) {
	/* framebuffer at top of RAM */
	if (phy_base >= _sys_info.total_phy_mem_size - PI5_FB_SIZE)
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
