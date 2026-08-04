#include <mm/mmu.h>
#include <stdint.h>
#include "hw_arch.h"

/*
 * Raspberry Pi 5 (BCM2712) boot stage:
 * build the early page tables and turn on the MMU.
 */

#define PDE_SHIFT     21
#define NUM_PAGE_DIRS 512
#define NUM_PAGE_TABLE_ENTRIES 4096

static __attribute__((__aligned__(PAGE_DIR_SIZE)))
page_dir_entry_t startup_page_dir[NUM_PAGE_DIRS] = { 0 };

static __attribute__((__aligned__(PAGE_DIR_SIZE)))
page_table_entry_t startup_page_table[NUM_PAGE_TABLE_ENTRIES] = { 0 };

static page_table_entry_t *entry_head;

static void boot_pgt_init(void){
	entry_head = startup_page_table;
	for(int i = 0; i < NUM_PAGE_DIRS; i++){
		startup_page_dir[i].EntryType = 0;
	}
}

static page_table_entry_t* get_free_page_table(void){
	if(entry_head >= &startup_page_table[NUM_PAGE_TABLE_ENTRIES]){
		/*no more free page table*/
		while(1);
	}

	page_table_entry_t *entry = entry_head;
	entry_head += 512;
	return entry;
}

static void set_boot_pgt(uint64_t virt, uint64_t phy, uint32_t len, int is_dev) {
	page_table_entry_t* entry;
	uint32_t l1 = PAGE_L1_INDEX(virt);
	uint32_t l2 = PAGE_L2_INDEX(virt);

	if( startup_page_dir[l1].EntryType == 0){
		entry = get_free_page_table();
		startup_page_dir[l1] = (page_dir_entry_t){
			.NSTable = 1,
			.EntryType = TYPE_TABLE,
			.Address = (uint64_t)entry >> 12,
			.AF = 1
		};
	}else{
		entry = startup_page_dir[l1].Address << 12;
	}

	phy  >>= PDE_SHIFT;
	len  >>= PDE_SHIFT;
	for (uint32_t idx =0 ; idx < len; idx++)
	{
		/* Each block descriptor (2 MB) */
		entry[l2] = (page_table_entry_t){
			.NSTable = 1,
			.EntryType = TYPE_BLOCK,
			.Address = phy << (21 - 12),
			.AF = 1,
			.SH = STAGE2_SH_OUTER_SHAREABLE,
			.S2AP = 0,
			.MemAttr = is_dev?MT_DEVICE_NGNRNE:MT_NORMAL,
		};
		l2++;
		phy++;
	}
}

extern void load_boot_pgt(uint32_t page_table);

/* early serial debug output, implemented in bsp/dbg.c */
void pi5_dbg_puts(const char* s);

void _boot_start(void) {
	boot_pgt_init();

	/* RAM: identity + kernel virtual window */
	set_boot_pgt(0, 0, 64*MB, 0);
	set_boot_pgt(KERNEL_BASE, 0, 64*MB, 0);

	/* BCM2712 peripherals (alias window), covers UART/mailbox/GIC */
	set_boot_pgt(MMIO_BASE, PI5_MMIO_PHY, PI5_MMIO_SIZE, 1);

	/* SD card host controller @ 0x1000FFF000 (one 2MB window) */
	set_boot_pgt(MMIO_BASE + PI5_EMMC_WIN_OFF, PI5_EMMC_PHY_WIN, 2*MB, 1);

	/* RP1 southbridge window for userland device drivers */
	set_boot_pgt(MMIO_BASE + PI5_RP1_WIN_OFF, PI5_RP1_PHY, PI5_RP1_SIZE, 1);

	load_boot_pgt(startup_page_dir);

	/* MMU is on now, serial debug is available */
	pi5_dbg_puts("[pi5] _boot_start: page tables ready, MMU enabled\n");
}
