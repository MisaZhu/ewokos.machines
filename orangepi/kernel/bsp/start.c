#include <mm/mmu.h>


#define PDE_SHIFT     20   // shift how many bits to get PDE index
#define KPDE_TYPE     0x02 // use "section" type for kernel page directory
#define AP_KO         0x01 // privilaged access, kernel: RW, user: no access

static __attribute__((__aligned__(PAGE_DIR_SIZE)))
volatile uint32_t startup_page_dir[PAGE_DIR_NUM] = { 0 };

// setup the boot page table with one-level section type paging : is_dev whether it is device memory
static void set_boot_pgt(uint32_t virt, uint32_t phy, uint32_t len, uint8_t is_dev) {
    (void)is_dev;
    volatile uint32_t idx;

    // convert all the parameters to indexes
    virt >>= PDE_SHIFT;
    phy  >>= PDE_SHIFT;
    len  >>= PDE_SHIFT;

    for (idx = 0; idx < len; idx++) {
        startup_page_dir[virt] = (phy << PDE_SHIFT) | AP_KO<< 10 | KPDE_TYPE; //section type, system RW 
        virt++;
        phy++;
    }
}

extern void load_boot_pgt(ewokos_addr_t page_table);

#define PIX_MMIO_SIZE 16*MB
#define DRAM_BASE	  0x40000000

void _boot_start(void) {
    set_boot_pgt(DRAM_BASE, DRAM_BASE, 64*MB, 0);
    set_boot_pgt(KERNEL_BASE, DRAM_BASE, 64*MB, 0);
    load_boot_pgt((ewokos_addr_t)startup_page_dir);
}
