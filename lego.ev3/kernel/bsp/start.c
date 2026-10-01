#include <mm/mmudef.h>

#define PDE_SHIFT     20   // shift how many bits to get PDE index

/*
 * ARM926EJ-S cache enable, split so the I-cache and D-cache are controlled
 * independently (this box has no serial console, so build flags are the only
 * bisect lever). u-boot's `go` hands over with the caches in whatever state it
 * left them; nothing in the kernel touched c1 before, so the box ran uncached.
 *
 * Hardware A/B on real EV3 silicon (cold-boot, repeated) established:
 *   I+D both OFF (0x2001) -> stable but slow
 *   I-cache ONLY (0x3001) -> stable, faster, but still too slow
 *   D-cache ON in WRITE-BACK (0x2005/0x3005) -> intermittent boot failure
 * so the D-cache was the culprit and it could not simply be turned on.
 *
 * The D-cache fault is now fixed at its source rather than avoided: the ARM926
 * D-cache is VIVT with no ASID and its translation-table walk is cached when D
 * is on, so Write-Back left dirty PTEs / DMA-shared data unpublished in DRAM and
 * the walker intermittently read stale memory. v5/mmu_pte_flags.c therefore maps
 * all normal memory Write-Through, No-Write-Allocate (C=1,B=0): every store
 * reaches DRAM immediately (fully coherent for the walker, DMA and cross-VA
 * readers) while read-caching - the dominant speedup - is retained. The stale
 * CLEAN-line alias across an address-space switch is covered by the full c7,c6,0
 * invalidate in set_translation_table_base(). With that in place BOTH caches are
 * enabled by default (fast AND coherent).
 *
 * Override to re-test other points of the matrix, e.g.:
 *   -DEV3_ENABLE_ICACHE=0 -DEV3_ENABLE_DCACHE=0  -> fully uncached
 *   -DEV3_ENABLE_DCACHE=0                        -> I-cache only
 */
#ifndef EV3_ENABLE_ICACHE
#define EV3_ENABLE_ICACHE 1
#endif
#ifndef EV3_ENABLE_DCACHE
#define EV3_ENABLE_DCACHE 1
#endif

static __attribute__((__aligned__(PAGE_DIR_SIZE))) 
uint32_t startup_page_dir[PAGE_DIR_NUM] = { 0 };

// setup the boot page table: dev_mem whether it is device memory
static void set_boot_pgt(uint32_t virt, uint32_t phy, uint32_t len, uint8_t is_dev) {
    (void)is_dev;
    volatile uint32_t idx;

    // convert all the parameters to indexes
    virt >>= PDE_SHIFT;
    phy  >>= PDE_SHIFT;
    len  >>= PDE_SHIFT;

    /**startup with one level section descripter(32 bits)
        2bits  0-2   : pagedir type, 2 for section type
        2bits  10-11 : AP, 2 for read only
        12bits 20-31 : base address of section (1M for one section)
     */
    for (idx = 0; idx < len; idx++) {
        startup_page_dir[virt] = (phy << PDE_SHIFT) | 2 | (2 << 10);
        virt++;
        phy++;
    }
}

static void load_boot_pgt(void) {
    volatile uint32_t val;

    // whatever the boot loader left in the caches belongs to its own mappings
    // (and the page dir written above may still sit dirty in it): write back
    // and drop everything before the new tables go live.  The c7,c14,3 loop
    // only removes dirty lines; c7,c6,0 drops the remaining clean lines so
    // no stale VIVT tag can alias with the new kernel mappings.
    __asm volatile(
        "1: MRC p15, 0, r15, c7, c14, 3\n"   // test, clean and invalidate one line
        "   bne 1b\n"
        "   mov r0, #0\n"
        "   MCR p15, 0, r0, c7, c6, 0\n"     // invalidate entire D-cache
        "   MCR p15, 0, r0, c7, c5, 0\n"     // invalidate icache
        "   MCR p15, 0, r0, c7, c10, 4\n"    // drain write buffer
        ::: "r0", "cc", "memory");

    // set domain access control: all domain will be checked for permission
    val = 0x55555555;
    __asm("MCR p15, 0, %[v], c3, c0, 0": :[v]"r" (val):);

    // set the kernel page table
    val = (uint32_t)&startup_page_dir;
    //__asm("MCR p15, 0, %[v], c2, c0, 1": :[v]"r" (val):);
    // set the user page table
    __asm("MCR p15, 0, %[v], c2, c0, 0": :[v]"r" (val):);

    // flush all TLB
    val = 0;
    __asm("MCR p15, 0, %[r], c8, c7, 0": :[r]"r" (val):);

    // ok, enable paging using read/modify/write
    __asm("MRC p15, 0, %[r], c1, c0, 0": [r]"=r" (val)::); //read
    val |= 0x2001; // enable MMU, high vector tbl
#if EV3_ENABLE_ICACHE
    val |= (1 << 12);            // I-cache
#endif
#if EV3_ENABLE_DCACHE
    val |= (1 << 2);             // D-cache
#endif
    __asm("MCR p15, 0, %[r], c1, c0, 0": :[r]"r" (val):); //write

    // flush all TLB
    val = 0;
    __asm("MCR p15, 0, %[r], c8, c7, 0": :[r]"r" (val):);
}

typedef struct{
    uint32_t mpsar;
    uint32_t mpear;
    uint32_t mppa;
    uint32_t resv
}mpu_t;

void _boot_start(void) {
    //disable MPU
    // refer ti AM1808 Technical Reference Manual (Rev. C)	page 95
    uint32_t* mpu_cfg = (uint32_t*)0x01e14004;
    mpu_t *mpu1 = (uint32_t*)0x01e14200;
    mpu_t *mpu2 = (uint32_t*)0x01e15100;
    mpu_t *mpu3 = (uint32_t*)0x01e15200;

    *mpu_cfg |= 0x1;

    for(int i = 0; i < 6; i++)
        mpu1[i].mppa = 0x0;

    mpu2[0].mppa = 0x0;

    for(int i = 0; i < 12; i++)
        mpu3[i].mppa = 0x0;

    //disable syscfg protate
    // refer ti AM1808 Technical Reference Manual (Rev. C)	page 211
    *(uint32_t *)(0x01C14038) = 0x83E70B13;
    *(uint32_t *)(0x01C1403C) = 0x95A4F1E0;

    set_boot_pgt(0xC0000000, 0xC0000000, 32*MB, 0);
    set_boot_pgt(KERNEL_BASE, 0xC0000000, 32*MB, 0);
    load_boot_pgt();
}
