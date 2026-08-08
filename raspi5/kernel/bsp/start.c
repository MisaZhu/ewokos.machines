#include <mm/mmu.h>
#include <mm/boot_pgt.h>
#include <stdint.h>
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
static boot_pgt_ctx_t boot_pgt = {
	.page_dir = startup_page_dir,
	.page_table_count = BOOT_PAGE_TABLE_COUNT,
	.page_tables = startup_page_tables,
};
#else
#define BOOT_PAGE_TABLE_COUNT 4
static __attribute__((__aligned__(PAGE_DIR_SIZE)))
page_table_entry_t startup_page_table[BOOT_PAGE_TABLE_COUNT * PAGE_DIR_NUM] = { 0 };
static boot_pgt_ctx_t boot_pgt = {
	.page_dir = startup_page_dir,
	.page_table_count = BOOT_PAGE_TABLE_COUNT,
	.page_tables = startup_page_table,
};
#endif

static void boot_pgt_init(void){
	boot_pgt_ctx_init(&boot_pgt);
}

static void set_boot_pgt(uint64_t virt, uint64_t phy, uint32_t len, int is_dev) {
	boot_pgt_map_range(&boot_pgt, virt, phy, len, is_dev);
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
