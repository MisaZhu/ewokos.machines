#include <mm/mmu.h>
#include <mm/boot_pgt.h>
#include <stdint.h>
#include "hw_arch.h"

/*
 * Q8B early boot stage: build page tables that map
 *   - the physical load address identity (the pre-MMU .init code runs there
 *     and keeps executing with the MMU on),
 *   - the high-half kernel alias (KERNEL_BASE + (phys - RAM_BASE)),
 *   - the low MMIO window (UART17 and friends) and the GICv3 window.
 *
 * This is deliberately a small, static set of tables: the real kernel VM is
 * built later by init_kernel_vm().
 */

#define NUM_PAGE_DIRS PAGE_DIR_NUM

static __attribute__((__aligned__(PAGE_DIR_SIZE)))
page_dir_entry_t startup_page_dir[NUM_PAGE_DIRS] = { 0 };

#if defined(PAGE_SIZE_16K)
#define BOOT_PAGE_TABLE_COUNT 32
static __attribute__((__aligned__(PAGE_DIR_SIZE)))
page_table_entry_t startup_page_tables[BOOT_PAGE_TABLE_COUNT][PAGE_DIR_NUM] = { 0 };
static boot_pgt_ctx_t boot_pgt = {
    .page_dir = startup_page_dir,
    .page_table_count = BOOT_PAGE_TABLE_COUNT,
    .page_tables = startup_page_tables,
};
#elif defined(PAGE_SIZE_64K)
#define BOOT_PAGE_TABLE_COUNT 16
static __attribute__((__aligned__(PAGE_DIR_SIZE)))
page_table_entry_t startup_page_tables[BOOT_PAGE_TABLE_COUNT][PAGE_DIR_NUM] = { 0 };
static boot_pgt_ctx_t boot_pgt = {
    .page_dir = startup_page_dir,
    .page_table_count = BOOT_PAGE_TABLE_COUNT,
    .page_tables = startup_page_tables,
};
#else
#define BOOT_PAGE_TABLE_COUNT 32
static __attribute__((__aligned__(PAGE_DIR_SIZE)))
page_table_entry_t startup_page_table[BOOT_PAGE_TABLE_COUNT * PAGE_DIR_NUM] = { 0 };
static boot_pgt_ctx_t boot_pgt = {
    .page_dir = startup_page_dir,
    .page_table_count = BOOT_PAGE_TABLE_COUNT,
    .page_tables = startup_page_table,
};
#endif

static void boot_pgt_init(void) {
    boot_pgt_ctx_init(&boot_pgt);
}

static void set_boot_pgt(uint64_t virt, uint64_t phy, uint32_t len, int is_dev) {
    boot_pgt_map_range(&boot_pgt, virt, phy, len, is_dev);
}

extern void load_boot_pgt(uint64_t page_table);
extern void q8b_install_vectors(void);

void _boot_start(void) {
    boot_pgt_init();

    /* identity window around the kernel: holds .init, bootinfo and stacks */
    set_boot_pgt(Q8B_EARLY_WIN_BASE, Q8B_EARLY_WIN_BASE,
            (uint32_t)Q8B_EARLY_WIN_SIZE, 0);

    /* high-half alias for the same window (V2P/P2V convention) */
    set_boot_pgt(KERNEL_BASE + Q8B_EARLY_WIN_VOFF, Q8B_EARLY_WIN_BASE,
            (uint32_t)Q8B_EARLY_WIN_SIZE, 0);

    /* low MMIO window: UART17 at 0x884000, GCC, TLMM, ... */
    set_boot_pgt(MMIO_BASE, Q8B_MMIO_PHY, (uint32_t)Q8B_MMIO_SIZE, 1);

    /* GICv3 window at its high-half slot */
    set_boot_pgt(MMIO_BASE + Q8B_GIC_WIN_OFF, Q8B_GIC_WIN_PHY,
            (uint32_t)Q8B_GIC_WIN_SIZE, 1);

    load_boot_pgt((uint64_t)startup_page_dir);

    /* The MMU is on now: install the real vector table (absolute literal,
     * see early_vectors.S) before calling into high-half C code. */
    q8b_install_vectors();
}

/* PSCI secondaries arrive only after the boot core has completed kernel VM
 * setup.  Reuse the immutable bootstrap table here; rebuilding it would race
 * with another core executing through the same mappings. */
void _boot_start_secondary(void) {
    load_boot_pgt((uint64_t)startup_page_dir);
    q8b_install_vectors();
}
