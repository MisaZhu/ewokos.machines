#include <kernel/hw_info.h>
#include <kernel/kernel.h>
#include <kernel/system.h>
#include <mm/mmu.h>
#include <mm/kalloc.h>
#include <kstring.h>
#include <stdbool.h>
#include <stdint.h>
#include "hw_arch.h"
#include "q8b_platform.h"

uint64_t _core_base_offset = 0;

uint64_t q8b_ramdisk_base = 0;
uint64_t q8b_ramdisk_size = 0;
uint64_t q8b_fb_base = 0;
uint64_t q8b_fb_size = 0;

static q8b_bootinfo_t _bi;
static int32_t _bi_valid = 0;

void arch_kernel_entry_early(void) {
}

q8b_bootinfo_t* q8b_bootinfo(void) {
    return (q8b_bootinfo_t*)(KERNEL_BASE +
            (Q8B_BOOTINFO_PHYS - Q8B_RAM_BASE));
}

static void bootinfo_load(void) {
    q8b_bootinfo_t* src = q8b_bootinfo();

    if (src->magic != Q8B_BOOTINFO_MAGIC ||
            src->version != Q8B_BOOTINFO_VERSION) {
        _bi_valid = 0;
        return;
    }
    memcpy(&_bi, src, sizeof(_bi));
    _bi_valid = 1;

    q8b_ramdisk_base = _bi.rd_base;
    q8b_ramdisk_size = _bi.rd_size;
    q8b_fb_base = _bi.fb_addr;
    q8b_fb_size = (uint64_t)_bi.fb_pitch * _bi.fb_height;
    /* SYS_MEM_MAP maps whole pages; round up so the final partial page is
     * inside the whitelisted range. */
    q8b_fb_size = (q8b_fb_size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

    if (q8b_fb_size > Q8B_FB_MAX_SIZE)
        q8b_fb_size = Q8B_FB_MAX_SIZE;
}

void sys_info_init_arch(void) {
    memset(&_sys_info, 0, sizeof(sys_info_t));
    bootinfo_load();

    strcpy(_sys_info.machine, "radxa-dragon-q8b");
    strcpy(_sys_info.arch, "aarch64");

    _sys_info.phy_offset = Q8B_RAM_BASE;

    uint64_t ram = _bi_valid ? _bi.ram_size : (4ULL * GB);
    if (ram < 256ULL * MB)
        ram = 4ULL * GB;

    _sys_info.total_phy_mem_size = ram;
    _sys_info.total_usable_mem_size = ram;
    if (_sys_info.total_usable_mem_size > Q8B_USABLE_LIMIT)
        _sys_info.total_usable_mem_size = Q8B_USABLE_LIMIT;
    if (_sys_info.total_usable_mem_size > MAX_USABLE_MEM_SIZE)
        _sys_info.total_usable_mem_size = MAX_USABLE_MEM_SIZE;

    _sys_info.mmio.phy_base = Q8B_MMIO_PHY;
    _sys_info.mmio.size = Q8B_MMIO_SIZE;
    if (_sys_info.mmio.size > MMIO_MAX_SIZE)
        _sys_info.mmio.size = MMIO_MAX_SIZE;

    _sys_info.allocable_phy_mem_top = _sys_info.phy_offset +
            _sys_info.total_usable_mem_size;
    _sys_info.sys_dma.size = 0;      /* generic sizing in sys_info_config() */
    _sys_info.shm_contig.size = 0;

#ifdef KERNEL_SMP
    _sys_info.cores = Q8B_CORES;
#else
    _sys_info.cores = 1;
#endif

    _sys_info.vector_base = (ewokos_addr_t)&interrupt_table_start;

    /* Publish the firmware hand-off to user space (GOP framebuffer and
     * RAM-disk rootfs). */
    q8b_platform_data_t pd;
    memset(&pd, 0, sizeof(pd));
    pd.fb.phy_base = _bi.fb_addr;
    pd.fb.width = _bi.fb_width;
    pd.fb.height = _bi.fb_height;
    pd.fb.pitch = _bi.fb_pitch;
    pd.fb.bpp = _bi.fb_bpp;
    pd.fb.format = _bi.fb_format;
    pd.rd.phy_base = _bi.rd_base;
    pd.rd.size = (uint32_t)_bi.rd_size;
    memcpy(_sys_info.platform_data, &pd,
            sizeof(pd) < PLATFORM_DATA_SIZE ? sizeof(pd) : PLATFORM_DATA_SIZE);

    if (q8b_ramdisk_size == 0)
        q8b_dbg_puts("q8b: no firmware rootfs, trying physical SD\n");
}

/*
 * The kernel VM keeps the low MMIO window at MMIO_BASE; the GIC lives in a
 * separate 16MB window 64MB above it, and the RAM-disk rootfs (when the
 * firmware put it outside the allocable direct map) is mapped at P2V().
 */
void arch_vm(page_dir_entry_t* vm) {
    map_pages_size(vm, MMIO_BASE + Q8B_GIC_WIN_OFF, Q8B_GIC_WIN_PHY,
            (uint32_t)Q8B_GIC_WIN_SIZE, AP_RW_D, PTE_ATTR_DEV);

#ifndef Q8B_QEMU_TEST
    map_pages_size(vm, MMIO_BASE + Q8B_SDCC_WIN_OFF, Q8B_SDCC_WIN_PHY,
            (uint32_t)Q8B_SDCC_WIN_SIZE, AP_RW_D, PTE_ATTR_DEV);
    map_pages_size(vm, MMIO_BASE + Q8B_TLMM_WIN_OFF, Q8B_TLMM_WIN_PHY,
            (uint32_t)Q8B_TLMM_WIN_SIZE, AP_RW_D, PTE_ATTR_DEV);
    map_pages_size(vm, MMIO_BASE + Q8B_RSC_WIN_OFF, Q8B_RSC_WIN_PHY,
            (uint32_t)Q8B_RSC_WIN_SIZE, AP_RW_D, PTE_ATTR_DEV);
    map_pages_size(vm, MMIO_BASE + Q8B_CMD_DB_WIN_OFF, Q8B_CMD_DB_WIN_PHY,
            (uint32_t)Q8B_CMD_DB_WIN_SIZE, AP_RW_D, PTE_ATTR_DEV);
#endif

    if (q8b_ramdisk_size == 0)
        return;

    /* Must be mapped here: init_kernel_vm() installs this VM and the very
     * next step (load_kernel_config) reads /etc/kernel.conf from the
     * RAM-disk rootfs, long before init_allocable_mem() maps the heap. */
    map_pages(vm, P2V(q8b_ramdisk_base), q8b_ramdisk_base,
            q8b_ramdisk_base + q8b_ramdisk_size,
            AP_RW_D, PTE_ATTR_WRBACK);
}

int32_t arch_clone_proc_vm(page_dir_entry_t* vm, page_dir_entry_t* kernel_vm) {
    (void)vm;
    (void)kernel_vm;
    return 0;
}

/*
 * Add only ranges that the final UEFI memory map marks reclaimable. Q8B DRAM
 * has a firmware-owned hole between its two RAM banks, so treating the whole
 * ram_base..ram_top span as one free range corrupts that memory once the
 * allocator grows beyond the first bank.
 */
void kalloc_arch(void) {
    typedef struct { ewokos_addr_t base, top; } rng_t;
    rng_t ex[Q8B_MAX_RESV + 3];
    uint32_t n = 0;

    ewokos_addr_t base = _sys_info.allocable_phy_mem_base;
    ewokos_addr_t top = _sys_info.allocable_phy_mem_top;
    if (top <= base)
        return;

#define ADD_EXCL(b, s) do { \
        ewokos_addr_t _b = (b); \
        ewokos_addr_t _s = (s); \
        if (_s != 0 && n < (sizeof(ex)/sizeof(ex[0]))) { \
            ex[n].base = _b; \
            ex[n].top = _b + _s; \
            n++; \
        } \
    } while (0)

    if (q8b_ramdisk_size)
        ADD_EXCL(q8b_ramdisk_base, q8b_ramdisk_size);
    if (q8b_fb_size)
        ADD_EXCL(q8b_fb_base, q8b_fb_size);
    if (_bi_valid) {
        for (uint32_t i = 0; i < _bi.resv_count &&
                i < Q8B_MAX_RESV; i++)
            ADD_EXCL(_bi.resv[i].base, _bi.resv[i].size);
    }

    /* insertion sort by base */
    for (uint32_t i = 1; i < n; i++) {
        rng_t key = ex[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && ex[j].base > key.base) {
            ex[j + 1] = ex[j];
            j--;
        }
        ex[j + 1] = key;
    }

    uint64_t map_bytes = (uint64_t)_bi.memmap_count * _bi.memmap_dsize;
    int32_t map_valid = _bi_valid && _bi.memmap_count != 0 &&
            _bi.memmap_dsize >= sizeof(q8b_efi_memdesc_t) &&
            _bi.memmap_addr >= Q8B_BOOTINFO_PHYS + 0x1000 &&
            _bi.memmap_addr < Q8B_BOOTINFO_PHYS + Q8B_BOOTINFO_SIZE &&
            map_bytes <= Q8B_BOOTINFO_SIZE - 0x1000 &&
            _bi.memmap_addr + map_bytes <=
                Q8B_BOOTINFO_PHYS + Q8B_BOOTINFO_SIZE;

    uint32_t ranges = map_valid ? _bi.memmap_count : 1;
    for (uint32_t m = 0; m < ranges; m++) {
        ewokos_addr_t range_base = base;
        ewokos_addr_t range_top = top;

        if (map_valid) {
            q8b_efi_memdesc_t* d = (q8b_efi_memdesc_t*)P2V(
                    _bi.memmap_addr + (uint64_t)m * _bi.memmap_dsize);
            if (!Q8B_MEM_USABLE(d->type) || d->pages == 0)
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
#undef ADD_EXCL
}

/*
 * Physical ranges user space may map with SYS_MEM_MAP:
 *   - the SoC low MMIO window (QUP/GENI, TLMM, GCC, UFS, PCIe, USB, ...),
 *   - the GOP framebuffer handed over by UEFI,
 *   - the firmware Command DB (RPMh resource addresses for the SD rails).
 * Arbitrary DRAM is deliberately not mappable.
 */
int32_t check_mem_map_arch(ewokos_addr_t phy_base, uint32_t size) {
    ewokos_addr_t phy_top = phy_base + (ewokos_addr_t)size;

    if (phy_top < phy_base)
        return -1;

    if (phy_top <= 0x20000000ULL)
        return 0;

    /* Q8B PCIe3a ECAM and the two outbound memory windows. Keep these
     * explicit: allowing every address above 4GB would expose arbitrary
     * DRAM and unrelated devices through SYS_MEM_MAP. */
    if (phy_base >= Q8B_PCIE3A_ECAM_PHY &&
            phy_top <= Q8B_PCIE3A_ECAM_PHY + Q8B_PCIE3A_ECAM_SIZE)
        return 0;
    if (phy_base >= Q8B_PCIE3A_MEM32_PHY &&
            phy_top <= Q8B_PCIE3A_MEM32_PHY + Q8B_PCIE3A_MEM32_SIZE)
        return 0;
    if (phy_base >= Q8B_PCIE3A_MEM64_PHY &&
            phy_top <= Q8B_PCIE3A_MEM64_PHY + Q8B_PCIE3A_MEM64_SIZE)
        return 0;

#ifndef Q8B_QEMU_TEST
    /* Firmware Command DB: user-space SDCC2 resolves the PM8350C SD-card
     * rails (ldoc6/ldoc9) itself during its own card probe. */
    if (phy_base >= Q8B_CMD_DB_WIN_PHY &&
            phy_top <= Q8B_CMD_DB_WIN_PHY + Q8B_CMD_DB_WIN_SIZE)
        return 0;
#endif

    /* RAM-disk rootfs: user-space sdfsd maps it read/write. */
    if (q8b_ramdisk_size != 0 &&
            phy_base >= q8b_ramdisk_base &&
            phy_top <= q8b_ramdisk_base + q8b_ramdisk_size)
        return 0;

    if (q8b_fb_size != 0 &&
            phy_base >= q8b_fb_base &&
            phy_top <= q8b_fb_base + q8b_fb_size)
        return 0;

    return -1;
}

int32_t mem_map_is_normal_ram_arch(ewokos_addr_t phy_base, uint32_t size) {
    if (q8b_ramdisk_size != 0 &&
            phy_base >= q8b_ramdisk_base &&
            phy_base + size <= q8b_ramdisk_base + q8b_ramdisk_size)
        return 1;

    if (q8b_fb_size != 0 &&
            phy_base >= q8b_fb_base &&
            phy_base + size <= q8b_fb_base + q8b_fb_size)
        return 1;
    return 0;
}
