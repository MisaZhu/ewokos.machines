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
    map_pages_size(vm, MMIO_BASE + Q8B_APPS_SMMU_OFF, Q8B_APPS_SMMU_PHY,
            (uint32_t)Q8B_APPS_SMMU_SIZE, AP_RW_D, PTE_ATTR_DEV);

    /*
     * Adreno 690 GPU complex (see hw_arch.h for the full physical layout
     * and Linux DT provenance).  Mapped as device memory so user space can
     * SYS_MEM_MAP the same window and drive GMU / CP / SMMU registers
     * without going through the kernel for every poke.  The mapping is
     * installed unconditionally: on a Q8B board the Adreno blocks are
     * always present at these addresses; the driver layer is what
     * decides whether to actually bring the GPU up.
     */
    map_pages_size(vm, MMIO_BASE + Q8B_GPU_WIN_OFF, Q8B_GPU_WIN_PHY,
            (uint32_t)Q8B_GPU_WIN_SIZE, AP_RW_D, PTE_ATTR_DEV);
    map_pages_size(vm, MMIO_BASE + Q8B_GPU_PDC_WIN_OFF, Q8B_GPU_PDC_WIN_PHY,
            (uint32_t)Q8B_GPU_PDC_WIN_SIZE, AP_RW_D, PTE_ATTR_DEV);
    map_pages_size(vm, MMIO_BASE + Q8B_GPU_ZAP_WIN_OFF, Q8B_GPU_ZAP_PHY,
            PAGE_SIZE, AP_RW_D, PTE_ATTR_NOCACHE);
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

/* Pools and the page allocator share every firmware/device exclusion. */
static uint32_t memory_exclusions(q8b_mem_range_t* ex) {
    uint32_t n = 0;
#define ADD_EXCL(b, s) do { \
        ewokos_addr_t _b = (b); \
        ewokos_addr_t _s = (s); \
        if (_s != 0 && n < (Q8B_MAX_RESV + 5)) { \
            ex[n].base = _b; \
            ex[n].top = _s > UINT64_MAX - _b ? UINT64_MAX : _b + _s; \
            n++; \
        } \
    } while (0)

    ADD_EXCL(_sys_info.sys_dma.phy_base, _sys_info.sys_dma.size);
    ADD_EXCL(_sys_info.shm_contig.phy_base, _sys_info.shm_contig.size);
    if (q8b_ramdisk_size)
        ADD_EXCL(q8b_ramdisk_base, q8b_ramdisk_size);
    if (q8b_fb_size)
        ADD_EXCL(q8b_fb_base, q8b_fb_size);
#ifndef Q8B_QEMU_TEST
    /* Reserve the entire CPU page; never allocate its adjacent 8KB. */
    ADD_EXCL(Q8B_GPU_ZAP_PHY, ALIGN_UP(Q8B_GPU_ZAP_SIZE, PAGE_SIZE));
#endif
    if (_bi_valid) {
        for (uint32_t i = 0; i < _bi.resv_count &&
                i < Q8B_MAX_RESV; i++)
            ADD_EXCL(_bi.resv[i].base, _bi.resv[i].size);
    }

#undef ADD_EXCL
    return n;
}

static int32_t memory_map_valid(void) {
    uint64_t bytes = (uint64_t)_bi.memmap_count * _bi.memmap_dsize;
    return _bi_valid && _bi.resv_count <= Q8B_MAX_RESV &&
            _bi.memmap_count != 0 &&
            _bi.memmap_dsize >= sizeof(q8b_efi_memdesc_t) &&
            (_bi.memmap_dsize & 7) == 0 && (_bi.memmap_addr & 7) == 0 &&
            _bi.memmap_addr >= Q8B_BOOTINFO_PHYS + 0x1000 &&
            _bi.memmap_addr < Q8B_BOOTINFO_PHYS + Q8B_BOOTINFO_SIZE &&
            bytes <= Q8B_BOOTINFO_SIZE - 0x1000 &&
            bytes <= Q8B_BOOTINFO_PHYS + Q8B_BOOTINFO_SIZE - _bi.memmap_addr;
}

static int32_t usable_descriptor(uint32_t m, uint64_t* base, uint64_t* top) {
    const q8b_efi_memdesc_t* d = (const q8b_efi_memdesc_t*)P2V(
            _bi.memmap_addr + (uint64_t)m * _bi.memmap_dsize);
    if (!Q8B_MEM_USABLE(d->type) || !d->pages ||
            d->pages > (UINT64_MAX - d->phys_start) / 4096ULL)
        return 0;
    *base = d->phys_start;
    *top = *base + d->pages * 4096ULL;
    return 1;
}

static uint64_t find_memory_pool(uint32_t size) {
    q8b_mem_range_t ex[Q8B_MAX_RESV + 5];
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
        top = q8b_pool_extend(top, _bi.memmap_count, usable_descriptor);
        if (base < _sys_info.allocable_phy_mem_base)
            base = _sys_info.allocable_phy_mem_base;
        if (top > limit)
            top = limit;
        uint64_t found = q8b_pool_candidate(base, top, size, PAGE_SIZE, ex, n);
        if (found && (!best || found < best))
            best = found;
    }
    return best;
}

int32_t arch_reserve_memory_pools(void) {
#ifdef Q8B_QEMU_TEST
    /* The QEMU direct-boot test has no UEFI handoff. */
    if (!_bi_valid)
        return 0;
#endif
    if (!memory_map_valid()) {
        kout_str("q8b: invalid UEFI memory map; refusing unsafe memory pools\n");
        halt();
    }
    _sys_info.sys_dma.phy_base = 0;
    _sys_info.shm_contig.phy_base = 0;
    _sys_info.sys_dma.phy_base = find_memory_pool(_sys_info.sys_dma.size);
    if (_sys_info.shm_contig.size)
        _sys_info.shm_contig.phy_base = find_memory_pool(_sys_info.shm_contig.size);
    if (!_sys_info.sys_dma.phy_base ||
            (_sys_info.shm_contig.size && !_sys_info.shm_contig.phy_base)) {
        kout_str("q8b: no firmware-safe DMA32 memory pools\n");
        halt();
    }
    _sys_info.shm_contig.v_base = 0;
    kout_str("q8b: DMA/SHM pools reserved from final UEFI memory map\n");
    return 1;
}

/* Only reclaim UEFI-approved RAM, excluding both reserved pools. */
void kalloc_arch(void) {
    q8b_mem_range_t ex[Q8B_MAX_RESV + 5];
    uint32_t n = memory_exclusions(ex);
    ewokos_addr_t base = _sys_info.allocable_phy_mem_base;
    ewokos_addr_t top = _sys_info.allocable_phy_mem_top;
    if (top <= base)
        return;

    /* insertion sort by base */
    for (uint32_t i = 1; i < n; i++) {
        q8b_mem_range_t key = ex[i];
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

    /* E-key Wi-Fi config space and assigned BAR apertures. */
    if (phy_base >= Q8B_PCIE4_DBI_PHY &&
            phy_top <= Q8B_PCIE4_DBI_PHY + Q8B_PCIE4_DBI_SIZE)
        return 0;
    if (phy_base >= Q8B_PCIE4_ECAM_PHY &&
            phy_top <= Q8B_PCIE4_ECAM_PHY + Q8B_PCIE4_ECAM_SIZE)
        return 0;
    if (phy_base >= Q8B_PCIE4_MEM32_PHY &&
            phy_top <= Q8B_PCIE4_MEM32_PHY + Q8B_PCIE4_MEM32_SIZE)
        return 0;
    if (phy_base >= Q8B_PCIE4_MEM64_PHY &&
            phy_top <= Q8B_PCIE4_MEM64_PHY + Q8B_PCIE4_MEM64_SIZE)
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

    /* PCIe2a / PCIe3b: the two M.2 NVMe slots (config space + BAR
     * apertures).  User-space nvmed discovers the controller through
     * ECAM and maps the assigned BAR through these windows. */
    if (phy_base >= Q8B_PCIE2A_ECAM_PHY &&
            phy_top <= Q8B_PCIE2A_ECAM_PHY + Q8B_PCIE2A_ECAM_SIZE)
        return 0;
    if (phy_base >= Q8B_PCIE2A_MEM32_PHY &&
            phy_top <= Q8B_PCIE2A_MEM32_PHY + Q8B_PCIE2A_MEM32_SIZE)
        return 0;
    if (phy_base >= Q8B_PCIE2A_MEM64_PHY &&
            phy_top <= Q8B_PCIE2A_MEM64_PHY + Q8B_PCIE2A_MEM64_SIZE)
        return 0;
    if (phy_base >= Q8B_PCIE3B_ECAM_PHY &&
            phy_top <= Q8B_PCIE3B_ECAM_PHY + Q8B_PCIE3B_ECAM_SIZE)
        return 0;
    if (phy_base >= Q8B_PCIE3B_MEM32_PHY &&
            phy_top <= Q8B_PCIE3B_MEM32_PHY + Q8B_PCIE3B_MEM32_SIZE)
        return 0;
    if (phy_base >= Q8B_PCIE3B_MEM64_PHY &&
            phy_top <= Q8B_PCIE3B_MEM64_PHY + Q8B_PCIE3B_MEM64_SIZE)
        return 0;

#ifndef Q8B_QEMU_TEST
    /* Firmware Command DB: user-space SDCC2 resolves the PM8350C SD-card
     * rails (ldoc6/ldoc9) itself during its own card probe. */
    if (phy_base >= Q8B_CMD_DB_WIN_PHY &&
            phy_top <= Q8B_CMD_DB_WIN_PHY + Q8B_CMD_DB_WIN_SIZE)
        return 0;

    /* zap staging is kernel-only through the restricted GPU PAS ABI. */
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
