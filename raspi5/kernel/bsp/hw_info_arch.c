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


void sys_info_init_arch(void) {
    memset(&_sys_info, 0, sizeof(sys_info_t));

    uint32_t board = bcm2712_board();
    uint32_t mem_size = bcm2712_mem_size();
    _sys_info.total_phy_mem_size = 2u*GB;

    switch(board) {
        case PI5_2G:   
            strcpy(_sys_info.machine, "raspberry-pi5-2g");
            _sys_info.total_phy_mem_size = 2u*GB;
            break;
        case PI5_4G:
            strcpy(_sys_info.machine, "raspberry-pi5-4g");
            _sys_info.total_phy_mem_size = 4ull*GB;
            break;
        case PI5_8G:   
            strcpy(_sys_info.machine, "raspberry-pi5-8g");
            _sys_info.total_phy_mem_size = 8ull*GB;
            break;
        case PI5_16G:   
            strcpy(_sys_info.machine, "raspberry-pi5-16g");
            _sys_info.total_phy_mem_size = 16ull*GB;
            break;
        case PI5_CM5_2G:   
            strcpy(_sys_info.machine, "raspberry-cm5-2g");
            _sys_info.total_phy_mem_size = 2u*GB;
            break;
        case PI5_CM5_4G:   
            strcpy(_sys_info.machine, "raspberry-cm5-4g");
            _sys_info.total_phy_mem_size = 4ull*GB;
            break;
        case PI5_CM5_8G:   
            strcpy(_sys_info.machine, "raspberry-cm5-8g");
            _sys_info.total_phy_mem_size = 8ull*GB;
            break;
        case PI5_CM5_16G:   
            strcpy(_sys_info.machine, "raspberry-cm5-16g");
            _sys_info.total_phy_mem_size = 16ull*GB;
            break;
        case PI5_PI500:
            strcpy(_sys_info.machine, "raspberry-pi500");
            break;
        default:   
            strcpy(_sys_info.machine, "raspberry-pi5");
            break;
    }

    if(board == PI5_UNKNOWN)
        ; /* unknown board revision, assuming Pi5 */

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

#ifdef PAGE_SIZE_64K
    /*
     * Pi 5 native HDMI allocates the scanout buffer from sys_dma. With 64KB
     * granules the allocation is rounded up more aggressively, and large
     * displays can leave too little headroom in the generic 32MB pool on 2GB
     * boards. Reserve a larger DMA window up front for display bring-up.
     */
    _sys_info.sys_dma.size = 64*MB;
#endif

    strcpy(_sys_info.arch, "aarch64");

    _sys_info.allocable_phy_mem_top = _sys_info.phy_offset +
            _sys_info.total_usable_mem_size;

#ifdef KERNEL_SMP
    _sys_info.cores = get_cpu_cores();
#else
    _sys_info.cores = 1;
#endif

    _sys_info.vector_base = (ewokos_addr_t)&interrupt_table_start;
}

void arch_vm(page_dir_entry_t* vm) {
    /*
     * Map MMIO windows using 4KB pages (TYPE_PAGE at L3), replacing the
     * previous 2MB block mappings (TYPE_BLOCK at L2). Use the common page-table
     * builder so all page sizes share the same walk and descriptor encoding.
     */


    /* Main MMIO window: 64 MB at 0x10_7C000000 */
    for (uint32_t i = 0; i < PI5_MMIO_SIZE / PAGE_SIZE; i++) {
        (void)map_page(vm, MMIO_BASE + i * PAGE_SIZE,
                PI5_MMIO_PHY + i * PAGE_SIZE, AP_RW_D, PTE_ATTR_DEV);
    }

    /* SDHCI hosts window: PI5_EMMC_WIN_SIZE at 0x10_00E00000 */
    for (uint32_t i = 0; i < PI5_EMMC_WIN_SIZE / PAGE_SIZE; i++) {
        (void)map_page(vm, MMIO_BASE + PI5_EMMC_WIN_OFF + i * PAGE_SIZE,
                PI5_EMMC_PHY_WIN + i * PAGE_SIZE, AP_RW_D, PTE_ATTR_DEV);
    }

    /* RP1 southbridge window: PI5_RP1_SIZE at 0x1F_00000000 */
    for (uint32_t i = 0; i < PI5_RP1_SIZE / PAGE_SIZE; i++) {
        (void)map_page(vm, MMIO_BASE + PI5_RP1_WIN_OFF + i * PAGE_SIZE,
                PI5_RP1_PHY + i * PAGE_SIZE, AP_RW_D, PTE_ATTR_DEV);
    }
}

int32_t arch_clone_proc_vm(page_dir_entry_t* vm, page_dir_entry_t* kernel_vm) {
    (void)vm;
    (void)kernel_vm;
    return 0;
}

#ifdef KERNEL_SMP
/*
 * Secondary core bring-up via PSCI CPU_ON (SMCCC, bsp/smccc-call.S).
 *
 * BCM2712 firmware parks cores 1-3 in the EL3 monitor (armstub8-2712),
 * enable-method is "psci" in the official device tree. The Pi3/4 style
 * spin-table at 0xE0 does not exist on Pi5, so poking a release address
 * and issuing sev can never wake a core here.
 */
#define PSCI_CPU_ON_AARCH64	0xC4000003UL

typedef struct {
    uint64_t a0;
    uint64_t a1;
    uint64_t a2;
    uint64_t a3;
} arm_smccc_res_t;

extern void arm_smccc_smc(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
        uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
        arm_smccc_res_t* res);

extern char __entry[];
void start_core(uint32_t core_id) {
    if(core_id >= _sys_info.cores)
        return;

    /* Cortex-A76 in DynamIQ: the core number lives in MPIDR Aff1 */
    uint64_t target_mpidr = (uint64_t)core_id << 8;
    arm_smccc_res_t res;

    /* the released core starts with MMU/caches off; make sure everything
     * written so far (kernel vm, _cpu_cores) is visible in RAM */
    flush_dcache();

    /* .init is linked at its load address, so __entry is the physical
     * entry point PSCI expects */
    arm_smccc_smc(PSCI_CPU_ON_AARCH64, target_mpidr,
            (uint64_t)__entry, 0, 0, 0, 0, 0, &res);
    /* res.a0 == 0 on success; the caller polls _cpu_cores[].actived */
}
#endif

void kalloc_arch(void) {
    ewokos_addr_t base = _sys_info.allocable_phy_mem_base;
    ewokos_addr_t top = _sys_info.allocable_phy_mem_top;

    /*
     * Punch the firmware framebuffer out of the heap. The VPU only reaches
     * ARM physical 0..1GB (bcm2712.dtsi dma-ranges), so the scan-out buffer
     * sits at the end of that window, not at the end of RAM.
     */
    if(base < PI5_FB_LOW_BASE && top > PI5_FB_LOW_BASE) {
        kalloc_append(P2V(base), P2V(PI5_FB_LOW_BASE));
        if(top > PI5_FB_LOW_TOP)
            kalloc_append(P2V(PI5_FB_LOW_TOP), P2V(top));
    }
    else {
        kalloc_append(P2V(base), P2V(top));
    }
}

int32_t check_mem_map_arch(ewokos_addr_t phy_base, uint32_t size) {
    ewokos_addr_t reset_page_base = ALIGN_DOWN(PI5_RESET_PAGE_PHY, PAGE_SIZE);
    ewokos_addr_t rescal_page_base = ALIGN_DOWN(PI5_RESCAL_PAGE_PHY, PAGE_SIZE);

    /*
     * Firmware framebuffer reserve, the only RAM a driver may map by physical
     * address. kalloc_arch() keeps this window out of the heap, so a scan-out
     * mapped from here cannot alias pages the allocator hands to somebody
     * else. A firmware buffer outside it is refused on purpose: the driver's
     * primary path owns its scan-out out of the sys_dma pool and does not need
     * this mapping at all.
     */
    if (phy_base >= PI5_FB_LOW_BASE && phy_base + size <= PI5_FB_LOW_TOP)
        return 0;
    if (_fb_actual_phy != 0
            && phy_base >= _fb_actual_phy
            && phy_base + size <= _fb_actual_end)
        return 0;

    /* main MMIO window: 64 MB at 0x10_7C000000 */
    if (phy_base >= PI5_MMIO_PHY &&
        phy_base + size <= PI5_MMIO_PHY + PI5_MMIO_SIZE)
        return 0;

    /* SDHCI hosts (EMMC) window: PI5_EMMC_WIN_SIZE at 0x10_00E00000,
     * carries the SD card host (0x1000FFF000) and the WiFi SDIO host
     * (0x1001100000) in one mapping. */
    if (phy_base >= PI5_EMMC_PHY_WIN &&
        phy_base + size <= PI5_EMMC_PHY_WIN + PI5_EMMC_WIN_SIZE)
        return 0;

    /* RP1 southbridge window: PI5_RP1_SIZE at 0x1F_00000000 */
    /* PCIe2 host bridge for the onboard RP1 southbridge. */
    /* pcie1 host bridge — external connector where an NVMe HAT is
     * attached (separate controller from pcie2/RP1).  nvme.c maps
     * a 64 KB window for the DBI + extended-config + misc registers. */
    if (phy_base >= PI5_PCIE1_PHY &&
        phy_base + size <= PI5_PCIE1_PHY + PI5_PCIE1_SIZE)
        return 0;

    /* pcie1 outbound memory window — 2 GB at 0x1b_80000000.
     * The NVMe device's BAR0 register file lives somewhere inside
     * this range; the driver maps a 64 KB sub-page through
     * SYS_MEM_MAP.  The whole window is whitelisted here because
     * BAR0 placement depends on the firmware/RC BAR allocation. */
    if (phy_base >= PI5_PCIE1_MEM_WIN_PHY &&
        phy_base + size <= PI5_PCIE1_MEM_WIN_PHY + PI5_PCIE1_MEM_WIN_SIZE)
        return 0;

    if (phy_base >= PI5_PCIE2_PHY &&
        phy_base + size <= PI5_PCIE2_PHY + PI5_PCIE2_SIZE)
        return 0;
    if (phy_base >= reset_page_base &&
        phy_base + size <= reset_page_base + PI5_RESET_PAGE_SIZE)
        return 0;
    if (phy_base >= rescal_page_base &&
        phy_base + size <= rescal_page_base + PI5_RESCAL_PAGE_SIZE)
        return 0;

    /* RP1 southbridge window: PI5_RP1_SIZE at 0x1F_00000000 */
    if (phy_base >= PI5_RP1_PHY &&
        phy_base + size <= PI5_RP1_PHY + PI5_RP1_SIZE)
        return 0;

    return -1;
}

int32_t mem_map_is_normal_ram_arch(ewokos_addr_t phy_base, uint32_t size) {
    ewokos_addr_t map_end = phy_base + size;

    if(map_end < phy_base)
        return 0;
    if(phy_base < _sys_info.allocable_phy_mem_base)
        return 0;
    if(map_end > _sys_info.allocable_phy_mem_top)
        return 0;
    return 1;
}
