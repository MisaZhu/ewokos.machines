#include <kernel/hw_info.h>
#include <kernel/kernel.h>
#include <kernel/system.h>
#include <mm/mmu.h>
#include <mm/kalloc.h>
#include <kstring.h>
#include <stdbool.h>
#include <stdint.h>
#include <kprintf.h>
#include "hw_arch.h"
#include "memory_pool.h"
#include "q8b_platform.h"

uint64_t _core_base_offset = 0;
/* Kernel serial console decision, taken from the loader's UARTC LSR probe
 * (bootinfo serial_flags bit0).  Set in bootinfo_load() before the first
 * kout so a board with no wired debug UART never drives a dead peripheral. */
int orin_serial_on = 0;

uint64_t orin_ramdisk_base = 0;
uint64_t orin_ramdisk_size = 0;
uint32_t orin_ramdisk_flags = 0;
uint64_t orin_fb_base = 0;
uint64_t orin_fb_size = 0;

static orin_bootinfo_t _bi;
static int32_t _bi_valid = 0;

void arch_kernel_entry_early(void) {
#ifdef ORIN_WATCHDOG_TEST
    /* TEST-only: arm the SBSA watchdog as early as possible (MMU is on, boot
     * tables map the WDT frames; bss is not cleared yet so use locals only).
     * Single-stage: WOR = cntfrq/2 * timeout, WS1 reset at `timeout`.  A hang
     * anywhere after this point resets the board.  Removable (WATCHDOG=1). */
    volatile uint32_t* ctrl =
            (volatile uint32_t*)(MMIO_BASE + ORIN_WDT_CTRL_WIN_OFF);
    uint64_t cntfrq, cntvct, wor, wcv;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(cntfrq));
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(cntvct));
    if (cntfrq == 0)
        cntfrq = 31250000ULL;
    wor = (cntfrq / 2ULL) * ORIN_WDT_BOOT_TIMEOUT;
    wcv = cntvct + wor;
    ctrl[ORIN_WDT_WOR / 4] = (uint32_t)wor;
    ctrl[(ORIN_WDT_WOR + 4) / 4] = (uint32_t)(wor >> 32);
    ctrl[ORIN_WDT_WCV / 4] = (uint32_t)wcv;
    ctrl[(ORIN_WDT_WCV + 4) / 4] = (uint32_t)(wcv >> 32);
    __asm__ volatile("dsb sy" ::: "memory");
    ctrl[ORIN_WDT_WCS / 4] = ORIN_WDT_WCS_EN;
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}

orin_bootinfo_t* orin_bootinfo(void) {
    return (orin_bootinfo_t*)(KERNEL_BASE +
            (ORIN_BOOTINFO_PHYS - ORIN_RAM_BASE));
}

static void bootinfo_load(void) {
    orin_bootinfo_t* src = orin_bootinfo();

    if (src->magic != ORIN_BOOTINFO_MAGIC ||
            src->version != ORIN_BOOTINFO_VERSION) {
        _bi_valid = 0;
        /* Bring-up robustness: a broken hand-off is exactly when diagnostics
         * matter most.  UARTC is always wired on the Orin Nano devkit (J14),
         * and every dbg wait is bounded, so default the console ON instead of
         * going silent.  A valid bootinfo still honours the loader's probe. */
        orin_serial_on = 1;
        return;
    }
    memcpy(&_bi, src, sizeof(_bi));
    _bi_valid = 1;

    orin_serial_on = (_bi.serial_flags & 1u) ? 1 : 0;
    orin_ramdisk_base = _bi.rd_base;
    orin_ramdisk_size = _bi.rd_size;
    orin_ramdisk_flags = _bi.rd_flags;
    orin_fb_base = _bi.fb_addr;
    orin_fb_size = (uint64_t)_bi.fb_pitch * _bi.fb_height;
    /* SYS_MEM_MAP maps whole pages; round up so the final partial page is
     * inside the whitelisted range. */
    orin_fb_size = (orin_fb_size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (orin_fb_size > ORIN_FB_MAX_SIZE)
        orin_fb_size = ORIN_FB_MAX_SIZE;
}

void sys_info_init_arch(void) {
    memset(&_sys_info, 0, sizeof(sys_info_t));
    bootinfo_load();

    /* Publish the real board model from the firmware DTB (handed over by the
     * loader); fall back to the board default when there is no /model. */
    if (_bi_valid && _bi.model[0] != 0) {
        uint32_t i = 0;
        for (; i + 1 < sizeof(_sys_info.machine) && _bi.model[i]; i++)
            _sys_info.machine[i] = _bi.model[i];
        _sys_info.machine[i] = 0;
    } else {
        strcpy(_sys_info.machine, "jetson-orin-nano");
    }
    strcpy(_sys_info.arch, "aarch64");

    _sys_info.phy_offset = ORIN_RAM_BASE;

    uint64_t ram = _bi_valid ? _bi.ram_size : (8ULL * GB);
    if (ram < 256ULL * MB)
        ram = 8ULL * GB;

    _sys_info.total_phy_mem_size = ram;
    _sys_info.total_usable_mem_size = ram;
    if (_sys_info.total_usable_mem_size > ORIN_USABLE_LIMIT)
        _sys_info.total_usable_mem_size = ORIN_USABLE_LIMIT;
    if (_sys_info.total_usable_mem_size > MAX_USABLE_MEM_SIZE)
        _sys_info.total_usable_mem_size = MAX_USABLE_MEM_SIZE;

    _sys_info.mmio.phy_base = ORIN_MMIO_PHY;
    _sys_info.mmio.size = ORIN_MMIO_SIZE;
    if (_sys_info.mmio.size > MMIO_MAX_SIZE)
        _sys_info.mmio.size = MMIO_MAX_SIZE;

    _sys_info.allocable_phy_mem_top = _sys_info.phy_offset +
            _sys_info.total_usable_mem_size;
    _sys_info.sys_dma.size = 0;      /* generic sizing in sys_info_config() */
    _sys_info.shm_contig.size = 0;

#ifdef KERNEL_SMP
    _sys_info.cores = ORIN_CORES;
#else
    _sys_info.cores = 1;
#endif

    _sys_info.vector_base = (ewokos_addr_t)&interrupt_table_start;

    /* Publish the firmware hand-off to user space (GOP framebuffer and
     * RAM-disk rootfs) using the shared q8b_platform_data_t layout so the
     * common init/sdfsd/uartd consume it unchanged. */
    q8b_platform_data_t pd;
    memset(&pd, 0, sizeof(pd));
    pd.fb.phy_base = _bi.fb_addr;
    pd.fb.width = _bi.fb_width;
    pd.fb.height = _bi.fb_height;
    pd.fb.pitch = _bi.fb_pitch;
    pd.fb.bpp = _bi.fb_bpp;
    pd.fb.format = _bi.fb_format;
    pd.fb._pad = _bi._pad_fb;
    pd.rd.phy_base = _bi.rd_base;
    pd.rd.size = (uint32_t)_bi.rd_size;
    pd.rd.flags = _bi.rd_flags;
    pd.rd.boot_source = _bi.boot_source;
    memcpy(_sys_info.platform_data, &pd,
            sizeof(pd) < PLATFORM_DATA_SIZE ? sizeof(pd) : PLATFORM_DATA_SIZE);

    /* Bring-up banner: the first hardware boot is debugged entirely over
     * UARTC, so dump the hand-off the kernel actually received. */
    orin_dbg_puts("orin: model=");
    orin_dbg_puts(_sys_info.machine);
    orin_dbg_puts(" cores=");
    orin_dbg_dec(_sys_info.cores);
    orin_dbg_puts(" ram=");
    orin_dbg_dec(_sys_info.total_usable_mem_size / MB);
    orin_dbg_puts("MB serial=");
    orin_dbg_dec(orin_serial_on);
    orin_dbg_puts("\n");
    if (_bi_valid) {
        orin_dbg_puts("orin: bootinfo rd=");
        orin_dbg_hex(_bi.rd_base);
        orin_dbg_puts("/");
        orin_dbg_dec(_bi.rd_size / KB);
        orin_dbg_puts("KB flags=");
        orin_dbg_hex(_bi.rd_flags);
        orin_dbg_puts(" resv=");
        orin_dbg_dec(_bi.resv_count);
        orin_dbg_puts(" fb=");
        orin_dbg_hex(_bi.fb_addr);
        orin_dbg_puts("\n");
    } else {
        orin_dbg_puts("orin: WARNING bootinfo invalid (magic/version)\n");
    }

    if (orin_ramdisk_size == 0)
        orin_dbg_puts("orin: no firmware ramdisk published\n");
}

/*
 * The kernel VM keeps the AON MMIO window at MMIO_BASE (installed by the
 * generic kernel map); the GIC lives in a dedicated window above it, and
 * the RAM-disk rootfs is mapped at P2V() so the boot reader and the early
 * kernel-config load can reach it before the heap direct map exists.
 */
void arch_vm(page_dir_entry_t* vm) {
    map_pages_size(vm, MMIO_BASE + ORIN_GIC_WIN_OFF, ORIN_GIC_WIN_PHY,
            (uint32_t)ORIN_GIC_WIN_SIZE, AP_RW_D, PTE_ATTR_DEV);

    if (orin_ramdisk_size == 0)
        return;

    /* Must be mapped here: init_kernel_vm() installs this VM and the very
     * next step (load_kernel_config) reads /etc/kernel/kernel.conf from the
     * RAM-disk rootfs, long before init_allocable_mem() maps the heap. */
    map_pages(vm, P2V(orin_ramdisk_base), orin_ramdisk_base,
            orin_ramdisk_base + orin_ramdisk_size,
            AP_RW_D, PTE_ATTR_WRBACK);
}

int32_t arch_clone_proc_vm(page_dir_entry_t* vm, page_dir_entry_t* kernel_vm) {
    (void)vm;
    (void)kernel_vm;
    return 0;
}

/* Pools and the page allocator share every firmware/device exclusion. */
static uint32_t memory_exclusions(orin_mem_range_t* ex) {
    uint32_t n = 0;
#define ADD_EXCL(b, s) do { \
        ewokos_addr_t _b = (b); \
        ewokos_addr_t _s = (s); \
        if (_s != 0 && n < (ORIN_MAX_RESV + 4)) { \
            ex[n].base = _b; \
            ex[n].top = _s > UINT64_MAX - _b ? UINT64_MAX : _b + _s; \
            n++; \
        } \
    } while (0)

    ADD_EXCL(_sys_info.sys_dma.phy_base, _sys_info.sys_dma.size);
    ADD_EXCL(_sys_info.shm_contig.phy_base, _sys_info.shm_contig.size);
    if (orin_ramdisk_size)
        ADD_EXCL(orin_ramdisk_base, orin_ramdisk_size);
    if (orin_fb_size)
        ADD_EXCL(orin_fb_base, orin_fb_size);
    if (_bi_valid) {
        for (uint32_t i = 0; i < _bi.resv_count &&
                i < ORIN_MAX_RESV; i++)
            ADD_EXCL(_bi.resv[i].base, _bi.resv[i].size);
    }

#undef ADD_EXCL
    return n;
}

static int32_t memory_map_valid(void) {
    uint64_t bytes = (uint64_t)_bi.memmap_count * _bi.memmap_dsize;
    return _bi_valid && _bi.resv_count <= ORIN_MAX_RESV &&
            _bi.memmap_count != 0 &&
            _bi.memmap_dsize >= sizeof(orin_efi_memdesc_t) &&
            (_bi.memmap_dsize & 7) == 0 && (_bi.memmap_addr & 7) == 0 &&
            _bi.memmap_addr >= ORIN_BOOTINFO_PHYS + 0x1000 &&
            _bi.memmap_addr < ORIN_BOOTINFO_PHYS + ORIN_BOOTINFO_SIZE &&
            bytes <= ORIN_BOOTINFO_SIZE - 0x1000 &&
            bytes <= ORIN_BOOTINFO_PHYS + ORIN_BOOTINFO_SIZE - _bi.memmap_addr;
}

static int32_t usable_descriptor(uint32_t m, uint64_t* base, uint64_t* top) {
    const orin_efi_memdesc_t* d = (const orin_efi_memdesc_t*)P2V(
            _bi.memmap_addr + (uint64_t)m * _bi.memmap_dsize);
    if (!ORIN_MEM_USABLE(d->type) || !d->pages ||
            d->pages > (UINT64_MAX - d->phys_start) / 4096ULL)
        return 0;
    *base = d->phys_start;
    *top = *base + d->pages * 4096ULL;
    return 1;
}

static uint64_t find_memory_pool(uint32_t size) {
    orin_mem_range_t ex[ORIN_MAX_RESV + 4];
    uint32_t n = memory_exclusions(ex);
    uint64_t best = 0;
    uint64_t limit = _sys_info.allocable_phy_mem_top;
    /* Keep both pools within the 32-bit physical reach of board devices. */
    if (limit > (1ULL << 32))
        limit = 1ULL << 32;
    for (uint32_t m = 0; m < _bi.memmap_count; m++) {
        uint64_t base, top;
        if (!usable_descriptor(m, &base, &top))
            continue;
        /* UEFI splits physically continuous RAM by allocation/type. Join
         * touching reclaimable descriptors before looking for a large pool;
         * descriptor order is not guaranteed. Never bridge a physical gap. */
        top = orin_pool_extend(top, _bi.memmap_count, usable_descriptor);
        if (base < _sys_info.allocable_phy_mem_base)
            base = _sys_info.allocable_phy_mem_base;
        if (top > limit)
            top = limit;
        uint64_t found = orin_pool_candidate(base, top, size, PAGE_SIZE, ex, n);
        if (found && (!best || found < best))
            best = found;
    }
    return best;
}

int32_t arch_reserve_memory_pools(void) {
    if (!memory_map_valid()) {
        kout_str("orin: invalid UEFI memory map; refusing unsafe memory pools\n");
        halt();
    }
    _sys_info.sys_dma.phy_base = 0;
    _sys_info.shm_contig.phy_base = 0;
    _sys_info.sys_dma.phy_base = find_memory_pool(_sys_info.sys_dma.size);
    if (_sys_info.shm_contig.size)
        _sys_info.shm_contig.phy_base = find_memory_pool(_sys_info.shm_contig.size);
    if (!_sys_info.sys_dma.phy_base ||
            (_sys_info.shm_contig.size && !_sys_info.shm_contig.phy_base)) {
        kout_str("orin: no firmware-safe DMA32 memory pools\n");
        halt();
    }
    _sys_info.shm_contig.v_base = 0;
    kout_str("orin: DMA/SHM pools reserved from final UEFI memory map\n");
    return 1;
}

/* Only reclaim UEFI-approved RAM, excluding both reserved pools. */
void kalloc_arch(void) {
    orin_mem_range_t ex[ORIN_MAX_RESV + 4];
    uint32_t n = memory_exclusions(ex);
    ewokos_addr_t base = _sys_info.allocable_phy_mem_base;
    ewokos_addr_t top = _sys_info.allocable_phy_mem_top;
    if (top <= base)
        return;

    /* insertion sort by base */
    for (uint32_t i = 1; i < n; i++) {
        orin_mem_range_t key = ex[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && ex[j].base > key.base) {
            ex[j + 1] = ex[j];
            j--;
        }
        ex[j + 1] = key;
    }

    int32_t map_valid = memory_map_valid();

    uint32_t ranges = map_valid ? _bi.memmap_count : 1;
    for (uint32_t m = 0; m < ranges; m++) {
        ewokos_addr_t range_base = base;
        ewokos_addr_t range_top = top;

        if (map_valid) {
            orin_efi_memdesc_t* d = (orin_efi_memdesc_t*)P2V(
                    _bi.memmap_addr + (uint64_t)m * _bi.memmap_dsize);
            if (!ORIN_MEM_USABLE(d->type) || d->pages == 0)
                continue;
            if (d->pages > UINT64_MAX / 4096ULL)
                continue;
            range_base = d->phys_start;
            range_top = range_base + d->pages * 4096ULL;
            if (range_top < range_base)
                continue;
            if (range_base < base)
                range_base = base;
            if (range_top > top)
                range_top = top;
            if (range_top <= range_base)
                continue;
        }

        ewokos_addr_t cur = range_base;
        for (uint32_t i = 0; i < n; i++) {
            if (ex[i].top <= cur || ex[i].base >= range_top)
                continue;
            if (ex[i].base > cur)
                kalloc_append(P2V(cur), P2V(ex[i].base));
            if (ex[i].top > cur)
                cur = ex[i].top;
        }
        if (cur < range_top)
            kalloc_append(P2V(cur), P2V(range_top));
    }
}

/*
 * Physical ranges user space may map with SYS_MEM_MAP:
 *   - the AON MMIO window (UARTC debug console),
 *   - the GICv3 window (diagnostics),
 *   - the GOP framebuffer handed over by UEFI,
 *   - the firmware RAM-disk rootfs.
 * Arbitrary DRAM is deliberately not mappable.
 */
int32_t check_mem_map_arch(ewokos_addr_t phy_base, uint32_t size) {
    ewokos_addr_t phy_top = phy_base + (ewokos_addr_t)size;
    if (phy_top < phy_base)
        return -1;

    if (phy_base >= ORIN_MMIO_PHY &&
            phy_top <= ORIN_MMIO_PHY + ORIN_MMIO_SIZE)
        return 0;

    if (phy_base >= ORIN_GIC_WIN_PHY &&
            phy_top <= ORIN_GIC_WIN_PHY + ORIN_GIC_WIN_SIZE)
        return 0;

    /* RAM-disk rootfs: init/sdfsd map it read/write. */
    if (orin_ramdisk_size != 0 &&
            phy_base >= orin_ramdisk_base &&
            phy_top <= orin_ramdisk_base + orin_ramdisk_size)
        return 0;

    if (orin_fb_size != 0 &&
            phy_base >= orin_fb_base &&
            phy_top <= orin_fb_base + orin_fb_size)
        return 0;

    /* PCIe C8 (RTL8168 ethernet): ECAM enumeration sub-range, the
     * non-prefetchable MMIO window holding the NIC BAR, and the RC
     * appl/config/DBI register windows.  Inherited from UEFI; user space
     * only reads config space and drives the MAC. */
    if (phy_base >= ORIN_PCIE_C8_ECAM_PHY &&
            phy_top <= ORIN_PCIE_C8_ECAM_PHY + ORIN_PCIE_C8_ECAM_MAP_SIZE)
        return 0;
    if (phy_base >= ORIN_PCIE_C8_MMIO_PHY &&
            phy_top <= ORIN_PCIE_C8_MMIO_PHY + ORIN_PCIE_C8_MMIO_SIZE)
        return 0;
    if (phy_base >= ORIN_PCIE_C8_CFG_PHY &&
            phy_top <= ORIN_PCIE_C8_CFG_PHY + 0x100000)
        return 0;
    if (phy_base >= ORIN_PCIE_C8_APPL_PHY &&
            phy_top <= ORIN_PCIE_C8_APPL_PHY + 0x20000)
        return 0;

    /* BPMP IVC: HSP_TOP0 doorbells + shared SRAM (non-cacheable IVC ring). */
    if (phy_base >= ORIN_HSP_TOP0_PHY &&
            phy_top <= ORIN_HSP_TOP0_PHY + ORIN_HSP_TOP0_SIZE)
        return 0;
    if (phy_base >= ORIN_BPMP_TX_PHY &&
            phy_top <= ORIN_BPMP_RX_PHY + ORIN_BPMP_SRAM_SIZE)
        return 0;

    /* NVDisplay register block (full-stack display path).  User space enables
     * the display clocks via BPMP, then programs head/window registers. */
    if (phy_base >= ORIN_NVDISPLAY_PHY &&
            phy_top <= ORIN_NVDISPLAY_PHY + ORIN_NVDISPLAY_SIZE)
        return 0;

    /* DCE (Display Control Engine) aperture.  T234 modeset is DCE-mediated;
     * the closed GOP reads head/window regs relative to this base.  Read-only
     * probe to confirm the GOP register base. */
    if (phy_base >= ORIN_DCE_PHY &&
            phy_top <= ORIN_DCE_PHY + ORIN_DCE_SIZE)
        return 0;

    /* PCIe C1 (M.2 Key-E) RC windows + ECAM enumeration sub-range. */
    if (phy_base >= ORIN_PCIE_C1_APPL_PHY &&
            phy_top <= ORIN_PCIE_C1_APPL_PHY + 0x20000)
        return 0;
    if (phy_base >= ORIN_PCIE_C1_CFG_PHY &&
            phy_top <= ORIN_PCIE_C1_CFG_PHY + 0x100000)
        return 0;
    if (phy_base >= ORIN_PCIE_C1_ECAM_PHY &&
            phy_top <= ORIN_PCIE_C1_ECAM_PHY + ORIN_PCIE_C8_ECAM_MAP_SIZE)
        return 0;
    if (phy_base >= ORIN_PCIE_C1_MMIO_PHY &&
            phy_top <= ORIN_PCIE_C1_MMIO_PHY + ORIN_PCIE_C1_MMIO_SIZE)
        return 0;

    /* P2U PHYs (C1 hsio_3, C8 gbe_2/3), 64KB each. */
    if (phy_base >= ORIN_C1_P2U_PHY && phy_top <= ORIN_C1_P2U_PHY + 0x10000)
        return 0;
    if (phy_base >= ORIN_C8_P2U0_PHY && phy_top <= ORIN_C8_P2U1_PHY + 0x10000)
        return 0;

    /* On-board peripherals: I2C, GPIO, pinmux, SDMMC1, USB xHCI/padctl. */
    if (phy_base >= ORIN_I2C_GEN1_PHY && phy_top <= ORIN_I2C_GEN1_PHY + 0x1000)
        return 0;
    if (phy_base >= ORIN_I2C_CAM_PHY && phy_top <= ORIN_I2C_CAM_PHY + 0x1000)
        return 0;
    if (phy_base >= ORIN_I2C_GEN2_PHY && phy_top <= ORIN_I2C_GEN8_PHY + 0x1000)
        return 0;
    if (phy_base >= ORIN_GPIO_MAIN_SEC_PHY &&
            phy_top <= ORIN_GPIO_MAIN_PHY + ORIN_GPIO_REG_SIZE)
        return 0;
    if (phy_base >= ORIN_GPIO_AON_SEC_PHY &&
            phy_top <= ORIN_GPIO_AON_PHY + ORIN_GPIO_REG_SIZE)
        return 0;
    if (phy_base >= ORIN_PINMUX_MAIN_PHY &&
            phy_top <= ORIN_PINMUX_MAIN_PHY + ORIN_PINMUX_MAIN_SIZE)
        return 0;
    if (phy_base >= ORIN_PINMUX_AON_PHY &&
            phy_top <= ORIN_PINMUX_AON_PHY + 0x10000)
        return 0;
    if (phy_base >= ORIN_SDMMC1_PHY && phy_top <= ORIN_SDMMC1_PHY + ORIN_SDMMC1_SIZE)
        return 0;
    if (phy_base >= ORIN_XUSB_FPCI_PHY && phy_top <= ORIN_XUSB_FPCI_PHY + 0x10000)
        return 0;
    if (phy_base >= ORIN_XUSB_HCD_PHY && phy_top <= ORIN_XUSB_HCD_PHY + ORIN_XUSB_HCD_SIZE)
        return 0;
    if (phy_base >= ORIN_XUSB_BAR2_PHY && phy_top <= ORIN_XUSB_BAR2_PHY + 0x10000)
        return 0;
    if (phy_base >= ORIN_XUSB_PADCTL_PHY &&
            phy_top <= ORIN_XUSB_PADCTL_PHY + ORIN_XUSB_PADCTL_SIZE)
        return 0;
    if (phy_base >= ORIN_XUSB_PADCTL_AO_PHY &&
            phy_top <= ORIN_XUSB_PADCTL_AO_PHY + 0x10000)
        return 0;

    /* TEST-only SBSA watchdog frames (watchdogd); harmless when unused. */
    if (phy_base >= ORIN_WDT_CTRL_PHY &&
            phy_top <= ORIN_WDT_CTRL_PHY + ORIN_WDT_FRAME_SIZE)
        return 0;
    if (phy_base >= ORIN_WDT_REFRESH_PHY &&
            phy_top <= ORIN_WDT_REFRESH_PHY + ORIN_WDT_FRAME_SIZE)
        return 0;

    return -1;
}

int32_t mem_map_is_normal_ram_arch(ewokos_addr_t phy_base, uint32_t size) {
    if (orin_ramdisk_size != 0 &&
            phy_base >= orin_ramdisk_base &&
            phy_base + size <= orin_ramdisk_base + orin_ramdisk_size)
        return 1;

    if (orin_fb_size != 0 &&
            phy_base >= orin_fb_base &&
            phy_base + size <= orin_fb_base + orin_fb_size)
        return 1;
    return 0;
}