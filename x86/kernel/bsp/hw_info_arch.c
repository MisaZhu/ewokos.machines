#include <kernel/hw_info.h>
#include <kernel/kernel.h>
#include <kernel/core.h>
#include <kernel/system.h>
#include <mm/mmu.h>
#include <mm/kalloc.h>
#include <kprintf.h>
#include <kstring.h>
#include <stdbool.h>
#include <stddef.h>
#include "x86_machine_smp.h"
#include "x86_bootinfo.h"

extern uint32_t interrupt_table_start;

/* arch/x64/boot.S uefi_entry 暂存的 bootinfo 物理地址 (0 = BIOS 引导) */
extern uint64_t x86_uefi_bootinfo;

/* 内存盘 rootfs 物理区间 (bootinfo v3, 0 = 未提供; bsp/sd.c 消费) */
uint64_t x86_rd_base = 0;
uint64_t x86_rd_size = 0;

ewokos_addr_t _core_base_offset = 0;

typedef struct __attribute__((packed)) {
    char signature[4];
    uint32_t config_table;
    uint8_t length;
    uint8_t spec_rev;
    uint8_t checksum;
    uint8_t feature1;
    uint8_t feature2;
    uint8_t feature3[3];
} x86_mp_floating_t;

typedef struct __attribute__((packed)) {
    char signature[4];
    uint16_t base_table_length;
    uint8_t spec_rev;
    uint8_t checksum;
    char oem_id[8];
    char product_id[12];
    uint32_t oem_table;
    uint16_t oem_table_size;
    uint16_t entry_count;
    uint32_t lapic_addr;
    uint16_t extended_table_length;
    uint8_t extended_table_checksum;
    uint8_t reserved;
} x86_mp_config_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t apic_id;
    uint8_t apic_version;
    uint8_t cpu_flags;
    uint32_t cpu_signature;
    uint32_t feature_flags;
    uint32_t reserved[2];
} x86_mp_cpu_entry_t;

#define X86_MP_CPU_ENTRY           0
#define X86_MP_CPU_ENABLED         0x01
#define X86_MP_CPU_BSP             0x02

static uint8_t x86_sum_bytes(const void *ptr, uint32_t size) {
    const uint8_t *p = (const uint8_t *)ptr;
    uint8_t sum = 0;

    for (uint32_t i = 0; i < size; i++) {
        sum = (uint8_t)(sum + p[i]);
    }
    return sum;
}

static uint16_t x86_bda_read16(uintptr_t addr) {
    uint16_t value;

    __asm__ volatile("movw (%1), %0" : "=r"(value) : "r"(addr) : "memory");
    return value;
}

static const x86_mp_floating_t* x86_find_mp_floating_in_range(uintptr_t start, uintptr_t end) {
    for (uintptr_t addr = start; (addr + sizeof(x86_mp_floating_t)) <= end; addr += 16) {
        const x86_mp_floating_t *mp = (const x86_mp_floating_t *)addr;

        if (memcmp((void *)mp->signature, (void *)"_MP_", 4) != 0) {
            continue;
        }
        if (mp->length == 0) {
            continue;
        }
        if (x86_sum_bytes(mp, (uint32_t)mp->length * 16) != 0) {
            continue;
        }
        return mp;
    }
    return NULL;
}

static const x86_mp_floating_t* x86_find_mp_floating(void) {
    uintptr_t ebda_addr = ((uintptr_t)x86_bda_read16(0x40E)) << 4;
    uintptr_t base_kb = (uintptr_t)x86_bda_read16(0x413);
    const x86_mp_floating_t *mp;

    if (ebda_addr != 0) {
        mp = x86_find_mp_floating_in_range(ebda_addr, ebda_addr + 1024);
        if (mp != NULL) {
            return mp;
        }
    }

    if (base_kb >= 1) {
        uintptr_t top_of_base = base_kb * 1024;
        mp = x86_find_mp_floating_in_range(top_of_base - 1024, top_of_base);
        if (mp != NULL) {
            return mp;
        }
    }

    return x86_find_mp_floating_in_range(0xF0000, 0x100000);
}

/* ---- BIOS 引导扇区 E820 内存图 (0000:5000, 见 bios_boot.S) ---- */
#define BIOS_E820_ADDR  0x5000ULL
#define BIOS_E820_MAGIC 0x45383230ULL   /* 'E820' */

static int32_t x86_bios_e820_memory(uint64_t *usable_out) {
    const volatile uint32_t *hdr = (const volatile uint32_t *)(uintptr_t)BIOS_E820_ADDR;
    uint64_t usable = 0;

    if (hdr[0] != (uint32_t)BIOS_E820_MAGIC) {
        return -1;
    }
    uint32_t count = hdr[1];
    if (count == 0 || count > 128) {
        return -1;
    }
    const volatile uint32_t *ent = (const volatile uint32_t *)(uintptr_t)(BIOS_E820_ADDR + 8);
    for (uint32_t i = 0; i < count; ++i, ent += 6) {
        uint64_t base = (uint64_t)ent[0] | ((uint64_t)ent[1] << 32);
        uint64_t len  = (uint64_t)ent[2] | ((uint64_t)ent[3] << 32);
        uint32_t type = ent[4];
        if (type == 1 && len > 0) {          /* AddressRangeMemory (usable) */
            usable += len;
        }
    }
    if (usable < 32 * MB) {
        return -1;                           /* 数据不合理, 弃用 */
    }
    *usable_out = usable;
    return 0;
}

/* ---- ACPI MADT 解析 (优先于 MP 表; BIOS: 低内存固件表, UEFI: bootinfo 拷贝) ---- */
#define ACPI_SIG_RSDP   0x2052545020445352ULL  /* "RSD PTR " */
#define ACPI_SIG_APIC   0x43495041             /* "APIC" */
#define MADT_ENTRY_LAPIC      0
#define MADT_ENTRY_X2APIC     9
#define MADT_LAPIC_ENABLED    0x01

typedef struct __attribute__((packed)) {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_addr;
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t ext_checksum;
    uint8_t reserved[3];
} acpi_rsdp_t;

/* 校验 ACPI 表头 (整个表按 length 求和 == 0) */
static int32_t acpi_table_valid(const volatile uint8_t *tbl) {
    uint8_t sum = 0;
    uint32_t len = *(const volatile uint32_t *)(tbl + 4);
    if (len < 36 || len > (1 * MB)) {
        return -1;
    }
    for (uint32_t i = 0; i < len; ++i) {
        sum = (uint8_t)(sum + tbl[i]);
    }
    return (sum == 0) ? 0 : -1;
}

static int32_t acpi_rsdp_valid(const volatile uint8_t *p) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < 20; ++i) {
        sum = (uint8_t)(sum + p[i]);
    }
    return (sum == 0 && *(const volatile uint64_t *)(p + 8) == ACPI_SIG_RSDP) ? 0 : -1;
}

/* 在 [start, end) 中扫描 RSDP */
static const volatile uint8_t *acpi_find_rsdp(uintptr_t start, uintptr_t end) {
    for (uintptr_t a = start; a + 8 <= end; a += 16) {
        if (*(const volatile uint64_t *)a == ACPI_SIG_RSDP &&
                acpi_rsdp_valid((const volatile uint8_t *)a) == 0) {
            return (const volatile uint8_t *)a;
        }
    }
    return NULL;
}

/* 从 RSDP 找 MADT, 返回表指针 (调用方保证 <1GB 可读), 长度经 *len_out */
static const volatile uint8_t *acpi_find_madt(const volatile uint8_t *rsdp, uint32_t *len_out) {
    uint32_t rsdt = *(const volatile uint32_t *)(rsdp + 16);
    uint8_t rsdp_rev = rsdp[15];
    const volatile uint32_t *rsdt_hdr = (const volatile uint32_t *)(uintptr_t)rsdt;
    uint32_t i;

    if (rsdt == 0 || rsdt >= (1 * MB)) {
        return NULL;
    }
    if (acpi_table_valid((const volatile uint8_t *)(uintptr_t)rsdt) != 0) {
        return NULL;
    }
    uint32_t rsdt_len = rsdt_hdr[1];
    uint32_t n = (rsdt_len - 36) / 4;
    for (i = 0; i < n; ++i) {
        uint32_t tbl = rsdt_hdr[9 + i];    /* 36 字节头之后 */
        if (tbl == 0 || tbl >= (1 * MB)) {
            continue;
        }
        if (*(const volatile uint32_t *)(uintptr_t)tbl == ACPI_SIG_APIC &&
                acpi_table_valid((const volatile uint8_t *)(uintptr_t)tbl) == 0) {
            *len_out = *(const volatile uint32_t *)(uintptr_t)(tbl + 4);
            return (const volatile uint8_t *)(uintptr_t)tbl;
        }
    }
    (void)rsdp_rev;
    return NULL;
}

/* 解析 MADT: 收集 enabled Local APIC, 填充 SMP 映射 */
static uint32_t x86_madt_apic_count;

static void x86_parse_madt_entries(const volatile uint8_t *madt, uint32_t len) {
    uint32_t off = 44;                       /* 36 头 + LAPIC 基址 + flags */
    uint32_t count = 0;
    uint8_t bsp_apic = 0xFF;
    uint8_t apics[CPU_MAX_CORES];

    memset(apics, 0xFF, sizeof(apics));
    x86_madt_apic_count = 0;
    x86_smp_mapping_reset();
    while (off + 2 <= len) {
        uint8_t type = madt[off];
        uint8_t elen = madt[off + 1];
        if (elen < 2 || off + elen > len) {
            break;
        }
        if (type == MADT_ENTRY_LAPIC && elen >= 8) {
            uint8_t apic_id = madt[off + 3];
            uint32_t flags = *(const volatile uint32_t *)(madt + off + 4);
            if ((flags & MADT_LAPIC_ENABLED) != 0 && count < CPU_MAX_CORES) {
                apics[count++] = apic_id;
            }
        }
        else if (type == MADT_ENTRY_X2APIC && elen >= 16) {
            /* 现代固件的 x2APIC MADT 条目 (type 9, 32 位 APIC ID): 真机
             * 上 type 0/type 9 可能混用甚至只有 type 9。内核以 xAPIC 模式
             * 运行 (见 bsp/ipi.c x86_lapic_mode_fixup), APIC ID >255 无法
             * 用 xAPIC 表示, 跳过并记日志 */
            uint32_t apic_id = *(const volatile uint32_t *)(madt + off + 4);
            uint32_t flags = *(const volatile uint32_t *)(madt + off + 12);
            if ((flags & MADT_LAPIC_ENABLED) != 0 && count < CPU_MAX_CORES) {
                if (apic_id <= 0xFF) {
                    apics[count++] = (uint8_t)apic_id;
                } else {
                    printf("smp: x2apic id %u > 255 skipped\n", apic_id);
                }
            }
        }
        off += elen;
    }
    x86_madt_apic_count = count;
    if (count == 0) {
        return;
    }
    /* BSP = 第一个 APIC (MADT 无显式 BSP 标记, 与 MP 表处理一致的做法) */
    bsp_apic = apics[0];
    x86_smp_set_apic_id(0, bsp_apic);
    for (uint32_t i = 1; i < count; ++i) {
        x86_smp_set_apic_id(i, apics[i]);
    }
}

/* UEFI bootinfo 拷贝的 MADT (拷贝位于 bootinfo 页内, 低位物理可读) */
static int32_t x86_uefi_madt_pending;
static const volatile uint8_t *x86_uefi_madt_ptr;
static uint32_t x86_uefi_madt_len;

static void x86_uefi_madt_set(const volatile uint8_t *ptr, uint32_t len) {
    x86_uefi_madt_ptr = ptr;
    x86_uefi_madt_len = len;
    x86_uefi_madt_pending = 1;
}

/* 统一入口: 先 UEFI 拷贝, 再 BIOS 低内存 ACPI 表; 都没有则回退 MP 表 */
static void x86_detect_mp_topology_inner(void);

static void x86_detect_smp_topology(void) {
    if (x86_uefi_madt_pending) {
        x86_parse_madt_entries(x86_uefi_madt_ptr, x86_uefi_madt_len);
        printf("smp: madt (uefi) %u apic(s)\n", x86_madt_apic_count);
        return;
    }
    const volatile uint8_t *rsdp = acpi_find_rsdp(0xE0000, 0x100000);
    uint32_t len = 0;
    if (rsdp != NULL) {
        const volatile uint8_t *madt = acpi_find_madt(rsdp, &len);
        if (madt != NULL && len >= 44) {
            x86_parse_madt_entries(madt, len);
            printf("smp: madt (bios) %u apic(s)\n", x86_madt_apic_count);
            return;
        }
    }
    printf("smp: mp-table fallback\n");
    x86_detect_mp_topology_inner();                /* MP 表回退 */
}

static void x86_detect_mp_topology_inner(void) {
    const x86_mp_floating_t *mp;
    const x86_mp_config_t *cfg;
    const uint8_t *entry;
    uint32_t count = 0;
    uint8_t bsp_apic = 0xFF;
    uint8_t apics[CPU_MAX_CORES];

    memset(apics, 0xFF, sizeof(apics));
    x86_smp_mapping_reset();
    mp = x86_find_mp_floating();
    if (mp == NULL || mp->config_table == 0) {
        return;
    }
    if (mp->config_table < 0x400 || mp->config_table >= (1024 * MB)) {
        return;
    }

    cfg = (const x86_mp_config_t *)(uintptr_t)mp->config_table;
    if (memcmp((void *)cfg->signature, (void *)"PCMP", 4) != 0) {
        return;
    }
    if (cfg->base_table_length < sizeof(x86_mp_config_t) ||
            cfg->base_table_length > 4096) {
        return;
    }
    if (x86_sum_bytes(cfg, cfg->base_table_length) != 0) {
        return;
    }

    entry = (const uint8_t *)(cfg + 1);
    for (uint32_t i = 0; i < cfg->entry_count && count < CPU_MAX_CORES; i++) {
        switch (entry[0]) {
        case X86_MP_CPU_ENTRY: {
            const x86_mp_cpu_entry_t *cpu = (const x86_mp_cpu_entry_t *)entry;
            if ((cpu->cpu_flags & X86_MP_CPU_ENABLED) != 0) {
                if ((cpu->cpu_flags & X86_MP_CPU_BSP) != 0) {
                    bsp_apic = cpu->apic_id;
                }
                apics[count++] = cpu->apic_id;
            }
            entry += sizeof(x86_mp_cpu_entry_t);
            break;
        }
        case 1:
        case 2:
        case 3:
        case 4:
            entry += 8;
            break;
        default:
            return;
        }
    }

    if (count == 0) {
        return;
    }

    if (bsp_apic == 0xFF) {
        bsp_apic = apics[0];
    }

    x86_smp_set_apic_id(0, bsp_apic);
    for (uint32_t i = 0, core_id = 1; i < count && core_id < CPU_MAX_CORES; i++) {
        if (apics[i] == bsp_apic) {
            continue;
        }
        x86_smp_set_apic_id(core_id++, apics[i]);
    }
}

/* ------------------------------------------------------------------
 * 内核 AHCI 引导支持: 现代 x86(及 q35 机型)没有 legacy IDE(0x1F0),
 * 根文件系统需经 AHCI DMA 读取。这里在启动最早阶段(boot_pml4 恒等
 * 映射仍生效)完成:
 *   1. PCI 配置空间探测 AHCI(class 0106)控制器, 记录 ABAR 物理地址;
 *   2. 把 ABAR 所在 2MB 区映射到 boot_pml4 的 VA 0xC0000000 区
 *      (PDPT[3] -> boot_pd_spare, 见 boot.S);
 *   3. arch_vm() 对内核正式页表做同样映射 —— 重映射前后 VA 不变,
 *      bsp/sd.c 的 AHCI 路径全程使用该 VA。
 * ------------------------------------------------------------------ */
uint64_t x86_ahci_abar_phys = 0;
uint64_t x86_nvme_bar_phys = 0;
#define X86_PCI_MMIO_VA   0xC0000000ULL
#define X86_NVME_VA       0xE0000000ULL   /* spare PD entry 256 (0xC0..+512MB) */

#define PCI_CFG_ADDR 0xCF8
#define PCI_CFG_DATA 0xCFC

static uint32_t x86_pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    outl(PCI_CFG_ADDR, 0x80000000u | ((uint32_t)bus << 16) |
            ((uint32_t)dev << 11) | ((uint32_t)func << 8) | (off & 0xFC));
    return inl(PCI_CFG_DATA);
}

extern uint64_t boot_pdpt;
extern uint64_t boot_pd_spare;

static void x86_early_map_pci_mmio(void) {
    uint64_t base;
    volatile uint64_t *pd;
    volatile uint64_t *pdpt = &boot_pdpt;
    uint32_t i;

    /* PCI 总线扫描: class 0106 (SATA/AHCI) 与 0108 (NVMe) */
    for (uint32_t bus = 0; bus < 8; bus++) {
        for (uint32_t dev = 0; dev < 32; dev++) {
            for (uint32_t fn = 0; fn < 8; fn++) {
                uint32_t id = x86_pci_read32(bus, dev, fn, 0x00);
                uint32_t cls;
                if ((id & 0xFFFF) == 0xFFFF) {
                    continue;
                }
                cls = x86_pci_read32(bus, dev, fn, 0x08) >> 16;
                if (cls != 0xFFFF >> 16) {
                    printf("sd: pci %u/%u/%u cls=%x\n", bus, dev, fn, cls);
                }
                if (cls == 0x0106 && x86_ahci_abar_phys == 0) {
                    uint32_t bar5 = x86_pci_read32(bus, dev, fn, 0x24);
                    if ((bar5 & 0x1) == 0 && (bar5 & ~0xFu) != 0) {
                        x86_ahci_abar_phys = bar5 & ~0xFu;
                    }
                } else if (cls == 0x0108 && x86_nvme_bar_phys == 0) {
                    uint32_t bar0 = x86_pci_read32(bus, dev, fn, 0x10);
                    if ((bar0 & 0x1) == 0 && (bar0 & ~0xFu) != 0) {
                        uint32_t hi = x86_pci_read32(bus, dev, fn, 0x14);
                        x86_nvme_bar_phys =
                            ((uint64_t)(hi & ~0xFu) << 32) | (bar0 & ~0xFu);
                    }
                }
            }
        }
    }

    /* boot_pml4: PDPT[3] -> boot_pd_spare。
     * entry 0..1  : AHCI ABAR 所在 4MB (2×2MB 大页)
     * entry 256   : NVMe BAR 所在 2MB (VA 0xE0000000) */
    pd = &boot_pd_spare;
    pdpt[3] = ((uint64_t)(uintptr_t)&boot_pd_spare) | 0x003;
    if (x86_ahci_abar_phys != 0) {
        base = x86_ahci_abar_phys & ~0x3FFFFFULL;
        for (i = 0; i < 2; i++) {
            pd[i] = (base + (uint64_t)i * 0x200000ULL) | 0x083; /* P|RW|PS(2MB) */
        }
    }
    if (x86_nvme_bar_phys != 0) {
        pd[256] = (x86_nvme_bar_phys & ~0x1FFFFFULL) | 0x083;
    }
    __asm__ volatile("invlpg (%0)" :: "r"(X86_PCI_MMIO_VA) : "memory");
    __asm__ volatile("invlpg (%0)" :: "r"(X86_NVME_VA) : "memory");

    if (x86_ahci_abar_phys != 0) {
        printf("sd: ahci abar %x\n", (uint32_t)x86_ahci_abar_phys);
    } else {
        printf("sd: no AHCI controller found\n");
    }
    if (x86_nvme_bar_phys != 0) {
        printf("sd: nvme bar %llx\n", (unsigned long long)x86_nvme_bar_phys);
    }
}

/* 解析 UEFI stub 传入的 bootinfo: EFI 内存图 → 内存大小, GOP → framebuffer。
 * 解析失败/BIOS 引导时保持默认值, 不影响既有 BIOS 路径。 */
static void x86_parse_bootinfo(void) {
    const x86_bootinfo_t *bi = (const x86_bootinfo_t *)(uintptr_t)x86_uefi_bootinfo;
    uint64_t usable_top = 0;

    if (bi == NULL || bi->magic != X86_BOOTINFO_MAGIC ||
            bi->version < 2 || bi->version > X86_BOOTINFO_VERSION) {
        return;
    }

    if (bi->memmap_addr != 0 && bi->memmap_dsize >= sizeof(x86_efi_memdesc_t)) {
        const uint8_t *base = (const uint8_t *)(uintptr_t)bi->memmap_addr;
        for (uint64_t i = 0; i < bi->memmap_count; i++) {
            const x86_efi_memdesc_t *d =
                (const x86_efi_memdesc_t *)(base + i * bi->memmap_dsize);
            if (!X86_EFI_MEM_USABLE(d->Type)) {
                continue;
            }
            uint64_t top = d->PhysicalStart + d->NumberOfPages * 4096;
            if (top > usable_top) {
                usable_top = top;
            }
        }
    }

    /* RAM 上限仍受 MAX_USABLE_MEM_SIZE 约束(下方统一 clamp) */
    if (usable_top >= 64 * MB) {
        _sys_info.total_phy_mem_size = usable_top;
    }

    if (bi->fb_addr != 0 && bi->fb_width > 0 && bi->fb_height > 0) {
        _sys_info.fb.phy_base = bi->fb_addr;
        _sys_info.fb.width = bi->fb_width;
        _sys_info.fb.height = bi->fb_height;
        _sys_info.fb.pitch = bi->fb_pitch;
        _sys_info.fb.bpp = bi->fb_bpp;
    }

    if (bi->version >= 2 && bi->madt_addr != 0 && bi->madt_len >= 44) {
        x86_uefi_madt_set((const volatile uint8_t *)(uintptr_t)bi->madt_addr,
                          bi->madt_len);
    }

    /* v3: 内存盘 rootfs。stub 已保证分配在 <512MB (AllocateMaxAddress),
     * 这里再校验区间落在可用 RAM 内且不压内核 (内核加载于 0x100000 起,
     * 镜像 <4MB), 否则忽略。 */
    if (bi->version >= 3 && bi->rd_base != 0 && bi->rd_size != 0) {
        uint64_t rd_end = bi->rd_base + bi->rd_size;
        if (rd_end <= _sys_info.total_phy_mem_size && bi->rd_base >= 8 * MB) {
            x86_rd_base = bi->rd_base;
            x86_rd_size = bi->rd_size;
            _sys_info.rd.phy_base = (ewokos_addr_t)bi->rd_base;
            _sys_info.rd.v_base = (ewokos_addr_t)X86_RAMDISK_VA(x86_rd_base & 0x1FFFFF);
            _sys_info.rd.size = (uint32_t)bi->rd_size;
        }
    }

    printf("boot: uefi bootinfo v%d, firmware %x, mem %dMB",
            bi->version, (uint32_t)bi->firmware_rev,
            (int32_t)(_sys_info.total_phy_mem_size / MB));
    if (_sys_info.fb.phy_base != 0) {
        printf(", fb %dx%d@%d\n", _sys_info.fb.width, _sys_info.fb.height,
                _sys_info.fb.bpp);
    } else {
        printf(", no fb\n");
    }
    if (x86_rd_size != 0) {
        printf("boot: ramdisk %dMB@%llx\n", (int32_t)(x86_rd_size / MB),
                (unsigned long long)x86_rd_base);
    }
}

void sys_info_init_arch(void) {
    memset(&_sys_info, 0, sizeof(sys_info_t));
    _sys_info.phy_offset = 0x00100000;
    _sys_info.vector_base = (ewokos_addr_t)&interrupt_table_start;
    _sys_info.total_phy_mem_size = 512 * MB;
    /* BIOS 引导: 引导扇区 E820 捕获 (0000:5000, 见 bios_boot.S) */
    {
        uint64_t e820_usable = 0;
        if (x86_bios_e820_memory(&e820_usable) == 0) {
            _sys_info.total_phy_mem_size = (ewokos_addr_t)e820_usable;
        }
    }
    /* UEFI 引导: 先用 bootinfo(EFI 内存图/GOP) 修正内存与 fb, 再派生 usable */
    x86_parse_bootinfo();
    _sys_info.total_usable_mem_size = _sys_info.total_phy_mem_size - _sys_info.phy_offset;
    if (_sys_info.total_usable_mem_size > (ewokos_addr_t)(MAX_USABLE_MEM_SIZE - _sys_info.phy_offset)) {
        _sys_info.total_usable_mem_size = (ewokos_addr_t)(MAX_USABLE_MEM_SIZE - _sys_info.phy_offset);
    }
    _sys_info.mmio.phy_base = 0xFD000000;
    _sys_info.mmio.size = 0x02000000;
    _sys_info.sys_dma.size = 16 * MB;
    _sys_info.machine[0] = 'x';
    _sys_info.machine[1] = '8';
    _sys_info.machine[2] = '6';
    _sys_info.machine[3] = '\0';
    _sys_info.arch[0] = 'x';
    _sys_info.arch[1] = '8';
    _sys_info.arch[2] = '6';
    _sys_info.arch[3] = '_';
    _sys_info.arch[4] = '6';
    _sys_info.arch[5] = '4';
    _sys_info.arch[6] = '\0';
    x86_early_map_pci_mmio();
    x86_detect_smp_topology();
    _sys_info.cores = get_cpu_cores();
    _sys_info.allocable_phy_mem_top = _sys_info.phy_offset + _sys_info.total_usable_mem_size;
}

/* 取/建下一级页表 (与 mmu_arch.c next_table 同手法) */
static page_table_entry_t *x86_vm_next_table(page_table_entry_t *tbl, uint32_t idx) {
    page_table_entry_t *t;

    if (tbl[idx].present) {
        return (page_table_entry_t *)(uintptr_t)
            P2V((ewokos_addr_t)(tbl[idx].Address << 12));
    }
    t = kalloc_page();
    if (t == NULL) {
        return NULL;
    }
    memset(t, 0, PAGE_TABLE_SIZE);
    tbl[idx].value = 0;
    tbl[idx].present = 1;
    tbl[idx].rw = 1;
    tbl[idx].us = 1;
    tbl[idx].Address = (uint64_t)V2P((ewokos_addr_t)t) >> 12;
    return (page_table_entry_t *)(uintptr_t)
        P2V((ewokos_addr_t)(tbl[idx].Address << 12));
}

/* 内存盘 2MB 大页映射到 X86_RAMDISK_VA 窗口 (x86_bootinfo.h):
 * 早期内核页表页池容不下 4KB 直映射 128MB 所需的 ~66 张 PT, 且
 * map_allocable_pages 会在同一 VA 区间做 4KB 直映射, 故 ramdisk 的
 * 内核态访问走独立 VA 窗口, 与 PCI MMIO 的 VA 窗口手法一致。 */
static void x86_map_ramdisk_vm(page_dir_entry_t* vm) {
    page_table_entry_t *pdpt;
    page_table_entry_t *pd;
    uint64_t pstart = x86_rd_base & ~0x1FFFFFULL;
    uint64_t pend = (x86_rd_base + x86_rd_size + 0x1FFFFFULL) & ~0x1FFFFFULL;
    uint64_t pa;
    uint32_t idx;

    if (x86_rd_size == 0) {
        return;
    }
    pdpt = x86_vm_next_table((page_table_entry_t *)vm,
            PAGE_PML4_INDEX(X86_RAMDISK_VA_BASE));
    if (pdpt == NULL) {
        return;
    }
    pd = x86_vm_next_table(pdpt, PAGE_PDPT_INDEX(X86_RAMDISK_VA_BASE));
    if (pd == NULL) {
        return;
    }
    /* PDPT[2] 的 PD 覆盖 VA 0x80000000-0xBFFFFFFF: 0xB0000000 窗口起始于
     * PD 索引 384, 绝不能从 0 写起 (pd[0..63] 是内核镜像/直映射的 4KB
     * PT 指针, 覆盖即取指落 ramdisk 数据) */
    idx = (X86_RAMDISK_VA_BASE >> 21) & 0x1FF;
    for (pa = pstart; pa < pend; pa += 0x200000) {
        if (idx >= 512) {
            return;
        }
        pd[idx].value = pa | 0x083;   /* Present|RW|PS(2MB), supervisor */
        idx++;
    }
}

/* GOP 帧缓冲 2MB 大页映射到 X86_FB_VA 窗口 (vgacon GOP 控制台):
 * UEFI 机器 0xB8000 文本区不接显示, 帧缓冲是唯一可见输出 */
static void x86_map_fb_vm(page_dir_entry_t* vm) {
    if (_sys_info.fb.phy_base == 0 || _sys_info.fb.pitch == 0 ||
            _sys_info.fb.height == 0 || _sys_info.fb.bpp < 16) {
        return;
    }
    uint64_t fb_size = (uint64_t)_sys_info.fb.pitch * _sys_info.fb.height;
    uint64_t pstart = _sys_info.fb.phy_base & ~0x1FFFFFULL;
    uint64_t pend = (_sys_info.fb.phy_base + fb_size + 0x1FFFFFULL) & ~0x1FFFFFULL;
    uint32_t idx = (X86_FB_VA >> 21) & 0x1FF;
    page_table_entry_t *pdpt = x86_vm_next_table((page_table_entry_t *)vm,
            PAGE_PML4_INDEX(X86_FB_VA));
    if (pdpt == NULL) {
        return;
    }
    page_table_entry_t *pd = x86_vm_next_table(pdpt, PAGE_PDPT_INDEX(X86_FB_VA));
    if (pd == NULL) {
        return;
    }
    for (uint64_t pa = pstart; pa < pend; pa += 0x200000) {
        if (idx >= 512) {
            break;
        }
        pd[idx].value = pa | 0x083;   /* Present|RW|PS(2MB), supervisor */
        idx++;
    }
}

void arch_vm(page_dir_entry_t* vm) {
    map_pages_size(vm, X86_AP_TRAMPOLINE_VADDR, X86_AP_TRAMPOLINE_PADDR,
            PAGE_SIZE, AP_RW_D, PTE_ATTR_WRBACK);
    /* VGA 文本缓冲区 (vgacon 第二输出汇点): 挂 PDPT[2] 共享区空闲位,
     * 内核与所有任务页表通写, 不参与 per-process 页表回收。
     * AP_RW_RW: console_handoff 后由用户态 vgacond (/dev/vga0) 接管续写 */
    map_pages_size(vm, X86_VGA_TEXT_VADDR, X86_VGA_TEXT_PHYS, PAGE_SIZE,
            AP_RW_RW, PTE_ATTR_DEV);
    x86_map_ramdisk_vm(vm);
    x86_map_fb_vm(vm);                   /* fb 映射进内核 VM (PDPT[2] 共享);
          vgacon_fb_ready 由 kernel.c 在 CR3 切换后调用 (boot_pml4 无 fb 窗口) */
    /* 与 boot_pml4 的 x86_early_map_pci_mmio 保持一致: 重映射后
     * VA 0xC0000000 仍指向 AHCI ABAR (4KB 页) */
    if (x86_ahci_abar_phys != 0) {
        map_pages_size(vm, X86_PCI_MMIO_VA, x86_ahci_abar_phys & ~0x3FFFFFULL,
                2 * 0x200000, AP_RW_D, PTE_ATTR_DEV);
    }
    if (x86_nvme_bar_phys != 0) {
        map_pages_size(vm, X86_NVME_VA, x86_nvme_bar_phys & ~0x1FFFFFULL,
                0x200000, AP_RW_D, PTE_ATTR_DEV);
    }
}

void kalloc_arch(void) {
    ewokos_addr_t base = _sys_info.allocable_phy_mem_base;
    ewokos_addr_t top = _sys_info.allocable_phy_mem_top;
    /* 内存盘 rootfs 占用的区间从空闲页表里挖掉 (page-ref 按 allocable
     * span 建立且 kalloc_append 只登记 free list, 中段空洞安全) */
    if (x86_rd_size != 0 &&
            x86_rd_base >= base && x86_rd_base + x86_rd_size <= top) {
        kalloc_append(P2V(base), P2V(x86_rd_base));
        kalloc_append(P2V(x86_rd_base + x86_rd_size), P2V(top));
        return;
    }
    kalloc_append(P2V(base), P2V(top));
}

int32_t check_mem_map_arch(ewokos_addr_t phy_base, uint32_t size) {
    ewokos_addr_t mmio_end;
    ewokos_addr_t map_end;
    if (size == 0) {
        return -1;
    }
    map_end = phy_base + size;
    if (map_end < phy_base) {
        return -1;
    }

    /* 内存盘 rootfs: 允许用户态 (sdfsd) 映射读写 */
    if (x86_rd_size != 0 &&
            phy_base >= x86_rd_base && map_end <= x86_rd_base + x86_rd_size) {
        return 0;
    }

    /* GOP 帧缓冲: QEMU 落在 0x80000000, 真机固件可能给 <0x80000000
     * (如 0x60000000) 或 4GB 以上 —— 4GB 以上由下方 PCI 洞规则放行,
     * 这里按 bootinfo 上报的 fb 区间精确放行 (大小 = pitch*height) */
    if (_sys_info.fb.phy_base != 0 && _sys_info.fb.pitch > 0 &&
            _sys_info.fb.height > 0) {
        uint64_t fb_size = (uint64_t)_sys_info.fb.pitch * _sys_info.fb.height;
        if ((uint64_t)phy_base >= _sys_info.fb.phy_base &&
                (uint64_t)map_end <= _sys_info.fb.phy_base + fb_size) {
            return 0;
        }
    }

    /* PCI MMIO 洞: QEMU pc 机型设备 BAR 从 0x80000000 起分配(64 位 BAR
     * 可能落在 4GB 以上, 如 0xC000000000), 真实平台固件亦普遍落在该范围
     * — 允许用户态驱动映射设备 BAR(上限 1TB)。 */
    if (phy_base >= 0x80000000ULL && map_end <= 0x10000000000ULL) {
        return 0;
    }

    if (_sys_info.mmio.size == 0) {
        return -1;
    }
    mmio_end = _sys_info.mmio.phy_base + _sys_info.mmio.size;
    if (phy_base >= _sys_info.mmio.phy_base && map_end <= mmio_end) {
        return 0;
    }
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
