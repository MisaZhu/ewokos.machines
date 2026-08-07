#include <mm/mmu.h>
#include <stdint.h>
#include <stddef.h>
#include "hw_arch.h"

/*
 * Raspberry Pi 5 (BCM2712) boot stage:
 * build the early page tables and turn on the MMU.
 */

#define NUM_PAGE_DIRS PAGE_DIR_NUM

static __attribute__((__aligned__(PAGE_DIR_SIZE)))
page_dir_entry_t startup_page_dir[NUM_PAGE_DIRS] = { 0 };

#ifdef PAGE_SIZE_16K
#define BOOT_PAGE_TABLE_COUNT 16
static __attribute__((__aligned__(PAGE_DIR_SIZE)))
page_table_entry_t startup_page_tables[BOOT_PAGE_TABLE_COUNT][PAGE_DIR_NUM] = { 0 };
static uint32_t startup_page_table_index;
#else
#define BOOT_PAGE_TABLE_COUNT 4
#define PDE_SHIFT     PAGE_BLOCK_SHIFT
#define NUM_PAGE_TABLE_ENTRIES (PAGE_DIR_NUM * BOOT_PAGE_TABLE_COUNT)
static __attribute__((__aligned__(PAGE_DIR_SIZE)))
page_table_entry_t startup_page_table[NUM_PAGE_TABLE_ENTRIES] = { 0 };
static page_table_entry_t *entry_head;
#endif

static void boot_zero_mem(void* p, size_t n) {
	volatile uint8_t* cur = (volatile uint8_t*)p;
	while(n-- > 0)
		*cur++ = 0;
}

static void boot_set_pte_flags(page_table_entry_t* pte, int is_dev) {
	pte->NSTable = 1;
	pte->EntryType = TYPE_PAGE;
	pte->AF = 1;
	pte->SH = STAGE2_SH_INNER_SHAREABLE;

	if(is_dev) {
		pte->PXN = 1;
		pte->UXN = 1;
		pte->SH = STAGE2_SH_OUTER_SHAREABLE;
		pte->MemAttr = MT_DEVICE_NGNRNE;
	}
	else {
		pte->MemAttr = MT_NORMAL;
	}
}

static void boot_pgt_init(void){
	boot_zero_mem(startup_page_dir, sizeof(startup_page_dir));
#ifdef PAGE_SIZE_16K
	startup_page_table_index = 0;
	boot_zero_mem(startup_page_tables, sizeof(startup_page_tables));
#else
	entry_head = startup_page_table;
	boot_zero_mem(startup_page_table, sizeof(startup_page_table));
#endif
}

static page_table_entry_t* get_free_page_table(void){
#ifdef PAGE_SIZE_16K
	if(startup_page_table_index >= BOOT_PAGE_TABLE_COUNT) {
		while(1);
	}

	return startup_page_tables[startup_page_table_index++];
#else
	if(entry_head >= &startup_page_table[NUM_PAGE_TABLE_ENTRIES]){
		while(1);
	}

	page_table_entry_t *entry = entry_head;
	entry_head += PAGE_DIR_NUM;
	return entry;
#endif
}

static void set_boot_pgt(uint64_t virt, uint64_t phy, uint32_t len, int is_dev) {
#ifdef PAGE_SIZE_16K
	uint64_t end = virt + len;

	while(virt < end) {
		uint32_t l1 = PAGE_L1_INDEX(virt);
		uint32_t l2 = PAGE_L2_INDEX(virt);
		uint32_t l3 = PAGE_L3_INDEX(virt);
		page_table_entry_t* l2_table;
		page_table_entry_t* l3_table;

		if(startup_page_dir[l1].EntryType == 0) {
			l2_table = get_free_page_table();
			boot_zero_mem(l2_table, PAGE_TABLE_SIZE);
			startup_page_dir[l1] = (page_dir_entry_t){
				.NSTable = 1,
				.EntryType = TYPE_TABLE,
				.Address = (uint64_t)l2_table >> PAGE_SHIFT,
				.AF = 1
			};
		}
		else {
			l2_table = (page_table_entry_t*)((uint64_t)startup_page_dir[l1].Address << PAGE_SHIFT);
		}

		if(l2_table[l2].EntryType == 0) {
			l3_table = get_free_page_table();
			boot_zero_mem(l3_table, PAGE_TABLE_SIZE);
			l2_table[l2] = (page_table_entry_t){
				.NSTable = 1,
				.EntryType = TYPE_TABLE,
				.Address = (uint64_t)l3_table >> PAGE_SHIFT,
				.AF = 1
			};
		}
		else {
			l3_table = (page_table_entry_t*)((uint64_t)l2_table[l2].Address << PAGE_SHIFT);
		}

		l3_table[l3].Address = phy >> PAGE_SHIFT;
		boot_set_pte_flags(&l3_table[l3], is_dev);

		virt += PAGE_SIZE;
		phy += PAGE_SIZE;
	}
#else
	page_table_entry_t* entry;
	uint32_t l1 = PAGE_L1_INDEX(virt);
	uint32_t l2 = PAGE_L2_INDEX(virt);

	if(startup_page_dir[l1].EntryType == 0) {
		entry = get_free_page_table();
		startup_page_dir[l1] = (page_dir_entry_t){
			.NSTable = 1,
			.EntryType = TYPE_TABLE,
			.Address = (uint64_t)entry >> PAGE_SHIFT,
			.AF = 1
		};
	}
	else {
		entry = (page_table_entry_t*)((uint64_t)startup_page_dir[l1].Address << PAGE_SHIFT);
	}

	phy  >>= PDE_SHIFT;
	len  >>= PDE_SHIFT;
	for(uint32_t idx = 0; idx < len; idx++) {
		entry[l2] = (page_table_entry_t){
			.NSTable = 1,
			.EntryType = TYPE_BLOCK,
			.Address = phy << (PDE_SHIFT - PAGE_SHIFT),
			.AF = 1,
			.SH = STAGE2_SH_OUTER_SHAREABLE,
			.S2AP = 0,
			.MemAttr = is_dev ? MT_DEVICE_NGNRNE : MT_NORMAL,
		};
		l2++;
		phy++;
	}
#endif
}

extern void load_boot_pgt(uint64_t page_table);

void _boot_start(void) {
	boot_pgt_init();

	/* RAM: identity + kernel virtual window */
	set_boot_pgt(0, 0, 64*MB, 0);
	set_boot_pgt(KERNEL_BASE, 0, 64*MB, 0);

	/* BCM2712 peripherals (alias window), covers UART/mailbox/GIC */
	set_boot_pgt(MMIO_BASE, PI5_MMIO_PHY, PI5_MMIO_SIZE, 1);

	/* SDHCI hosts: SD card @ 0x1000FFF000, WiFi SDIO @ 0x1001100000 */
	set_boot_pgt(MMIO_BASE + PI5_EMMC_WIN_OFF, PI5_EMMC_PHY_WIN, PI5_EMMC_WIN_SIZE, 1);

	/* RP1 southbridge window for userland device drivers */
	set_boot_pgt(MMIO_BASE + PI5_RP1_WIN_OFF, PI5_RP1_PHY, PI5_RP1_SIZE, 1);

	load_boot_pgt((uint64_t)startup_page_dir);

	/* MMU is on now */
}
