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

uint32_t _core_base_offset = 0;

void pi5_dbg_puts(const char* s);
void pi5_dbg_putc(char c);

/*
 * Set a 2MB block descriptor that maps a virtual address to a 64-bit
 * physical address.  map_page() accepts only uint32_t physical, so the
 * MMIO/EMMC/RP1 windows (all above 4GB on BCM2712) need this helper.
 */
static void set_block_2mb(page_dir_entry_t* vm, uint32_t vaddr, uint64_t phy) {
	uint32_t l1 = PAGE_L1_INDEX(vaddr);
	uint32_t l2 = PAGE_L2_INDEX(vaddr);

	/*
	 * The main MMIO window (64 MB) may not cover the EMMC / RP1 windows
	 * which sit right above it, so the L1 entry may not exist yet.
	 * Allocate an L2 table if needed (the same way map_page does).
	 */
	if(vm[l1].EntryType == 0) {
		page_table_entry_t* l2_table = (page_table_entry_t*)kalloc4k();
		if(l2_table == NULL)
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
	phy >>= 21;
	l2_table[l2] = (page_table_entry_t){
		.NSTable = 1,
		.EntryType = TYPE_BLOCK,
		.Address = phy << 9,
		.AF = 1,
		.SH = STAGE2_SH_OUTER_SHAREABLE,
		.S2AP = 0,
		.MemAttr = MT_DEVICE_NGNRNE,
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

	pi5_dbg_puts("[pi5] sys_info: board=");
	pi5_dbg_puts(_sys_info.machine);
	pi5_dbg_puts("\n");

	if(board == PI5_UNKNOWN)
		pi5_dbg_puts("[pi5] WARN: unknown board revision, assuming Pi5\n");

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
	_sys_info.cores = get_cpu_cores();
#else
	_sys_info.cores = 1;
#endif

	_sys_info.vector_base = (ewokos_addr_t)&interrupt_table_start;
}

void arch_vm(page_dir_entry_t* vm) {
	/*
	 * The generic kernel code maps the MMIO window via map_pages_size()
	 * which takes ewokos_addr_t (uint32_t), truncating PI5_MMIO_PHY
	 * from 0x10_7C000000 to 0x7C000000.  Replace those mappings here
	 * with 2 MB block descriptors carrying the full 64-bit physical
	 * addresses.
	 */
	for(uint32_t i = 0; i < PI5_MMIO_SIZE/(2*MB); i++) {
		set_block_2mb(vm, MMIO_BASE + i*(2*MB), PI5_MMIO_PHY + i*(2*MB));
	}

	/* SD host controller 2 MB window (0x10_00E00000) */
	set_block_2mb(vm, MMIO_BASE + PI5_EMMC_WIN_OFF, PI5_EMMC_PHY_WIN);

	/* RP1 southbridge 32 MB window (0x1F_00000000) */
	for(uint32_t i = 0; i < PI5_RP1_SIZE/(2*MB); i++) {
		set_block_2mb(vm, MMIO_BASE + PI5_RP1_WIN_OFF + i*(2*MB),
		              PI5_RP1_PHY + i*(2*MB));
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
	if(phy_base >= _sys_info.total_phy_mem_size - PI5_FB_SIZE) /* framebuffer block */
		return 0;
	if(phy_base >= _sys_info.mmio.phy_base && size <= _sys_info.mmio.size)
		return 0;
	return -1;
}
