#ifndef Q8B_HW_ARCH_H
#define Q8B_HW_ARCH_H

#include <stdint.h>
#include <mm/mmu.h>

/*
 * Radxa Dragon Q8B / Qualcomm SC8280XP hardware map.
 *
 * Boot chain: on-board SPI NOR EDK2 UEFI -> BOOTAA64.EFI (our stub) ->
 * kernel.bin copied to Q8B_KERNEL_PHYS.  There is no U-Boot.
 */

#ifdef Q8B_QEMU_TEST
/* QEMU virt smoke test: PL011 at 0x09000000, GICv2 at 0x08000000. */
#define Q8B_RAM_BASE        0x40000000ULL
#define Q8B_KERNEL_PHYS     0x80080000ULL
#define Q8B_BOOTINFO_PHYS   0x80070000ULL
#define Q8B_BOOTINFO_SIZE   0x10000ULL

#define Q8B_EARLY_WIN_BASE  0x80000000ULL
#define Q8B_EARLY_WIN_SIZE  (64*MB)
#define Q8B_EARLY_WIN_VOFF  (Q8B_EARLY_WIN_BASE - Q8B_RAM_BASE)

#define Q8B_MMIO_PHY        0x00000000ULL
#define Q8B_MMIO_SIZE       (64*MB)
/* QEMU virt puts the GIC at 0x08000000 and PL011 at 0x09000000, beyond the
 * 64MB QCOM low window; map a second 32MB window at +64MB. */
#define Q8B_UART17_PHY      0x09000000ULL
#define Q8B_UART17_OFF      (0x04000000 + 0x01000000)
#define Q8B_UART17_IRQ      33
#define Q8B_UART_BAUD       115200

#define Q8B_GIC_WIN_PHY     0x08000000ULL
#define Q8B_GIC_WIN_SIZE    (32*MB)
#define Q8B_GIC_WIN_OFF     0x04000000
#define Q8B_GICD_PHY        0x08000000ULL
#define Q8B_GICC_PHY        0x08010000ULL
#define Q8B_GICR_PHY        0ULL
#define Q8B_GICR_STRIDE     0ULL
#define Q8B_GIC_ITS_PHY     0ULL
#define Q8B_GIC_TIMER_IRQ   27
#define Q8B_GIC_IPI_IRQ     0
#define Q8B_CORES           1

#define Q8B_USABLE_LIMIT    (1ULL*GB)
#define Q8B_FB_MAX_SIZE     (64*MB)
#else
#define Q8B_RAM_BASE        0x80000000ULL
#define Q8B_KERNEL_PHYS     0x90080000ULL   /* RAM base + 256MB */
#define Q8B_BOOTINFO_PHYS   0x90070000ULL   /* 64KB reserved below kernel */
#define Q8B_BOOTINFO_SIZE   0x10000ULL

/* Early boot maps a 64MB window around the kernel.  The UEFI loader itself
 * is typically loaded near 0x80080000, so the kernel lives 128MB higher. */
#define Q8B_EARLY_WIN_BASE  0x90000000ULL
#define Q8B_EARLY_WIN_SIZE  (64*MB)
#define Q8B_EARLY_WIN_VOFF  (Q8B_EARLY_WIN_BASE - Q8B_RAM_BASE)

/* Main low MMIO window (QUP/GENI, GCC, TLMM, TCSR, ...) */
#define Q8B_MMIO_PHY        0x00000000ULL
#define Q8B_MMIO_SIZE       (64*MB)

/* GENI debug UART (uart17 in sc8280xp.dtsi, 40-pin header TX/RX) */
#define Q8B_UART17_PHY      0x00884000ULL
#define Q8B_UART17_OFF      0x00884000     /* offset in Q8B_MMIO_PHY window */
#define Q8B_UART17_IRQ      615            /* GIC SPI 583 => INTID 32+583 */
#define Q8B_UART_BAUD       115200

/* GICv3: distributor + one redistributor region (8 cores, 0x20000 stride) */
#define Q8B_GIC_WIN_PHY     0x17000000ULL
#define Q8B_GIC_WIN_SIZE    (16*MB)
#define Q8B_GIC_WIN_OFF     0x04000000     /* virtual offset from MMIO_BASE */
#define Q8B_GICD_PHY        0x17a00000ULL
#define Q8B_GICR_PHY        0x17a60000ULL
#define Q8B_GICR_STRIDE     0x20000ULL
#define Q8B_GIC_ITS_PHY     0x17a40000ULL

/* SDCC2 and TLMM sit above the 64MB low-MMIO window. Keep dedicated,
 * contiguous virtual windows below MMIO_MAX_SIZE rather than expanding the
 * physical low window across unrelated address holes. */
#define Q8B_SDCC_WIN_PHY    0x08800000ULL
#define Q8B_SDCC_WIN_SIZE   (1*MB)
#define Q8B_SDCC_WIN_OFF    0x05000000ULL
#define Q8B_SDCC2_PHY       0x08804000ULL
#define Q8B_SDCC2_OFF       (Q8B_SDCC_WIN_OFF + 0x4000ULL)

#define Q8B_TLMM_WIN_PHY    0x0f100000ULL
#define Q8B_TLMM_WIN_SIZE   (3*MB)
#define Q8B_TLMM_WIN_OFF    0x05100000ULL

/* RPMh drv-2 controls the PM8350C SD-card rails.  Command DB is firmware
 * populated reserved RAM containing the RPMh resource addresses. */
#define Q8B_RSC_WIN_PHY     0x18220000ULL
#define Q8B_RSC_WIN_SIZE    0x00010000ULL
#define Q8B_RSC_WIN_OFF     0x05400000ULL

#define Q8B_CMD_DB_WIN_PHY  0x80860000ULL
#define Q8B_CMD_DB_WIN_SIZE 0x00020000ULL
#define Q8B_CMD_DB_WIN_OFF  0x05500000ULL

#define Q8B_GIC_TIMER_IRQ   27             /* virtual timer PPI (CNTV) */
#define Q8B_GIC_IPI_IRQ     0              /* SGI0 */

#define Q8B_CORES           8

/* The 8GB physical span contains firmware-owned holes. kalloc_arch() filters
 * it through the final UEFI memory map before publishing pages. */
#define Q8B_USABLE_LIMIT    (8ULL*GB)

/* Firmware framebuffer is mapped by user space (SYS_MEM_MAP); allow a
 * generous window in check_mem_map_arch(). */
#define Q8B_FB_MAX_SIZE     (64*MB)

#endif

/* PCIe3a outbound apertures used by the on-board TC9563/QPS615 switch. */
#define Q8B_PCIE3A_ECAM_PHY      0x480000000ULL
#define Q8B_PCIE3A_ECAM_SIZE     0x10000000ULL
#define Q8B_PCIE3A_MEM32_PHY     0x490000000ULL
#define Q8B_PCIE3A_MEM32_SIZE    0x10000000ULL
#define Q8B_PCIE3A_MEM64_PHY     0x4a0000000ULL
#define Q8B_PCIE3A_MEM64_SIZE    0x20000000ULL

/*
 * PCIe2a (M.2 M-key, Gen3 x4) and PCIe3b (M.2 M-key, Gen3 x2) are the two
 * NVMe slots.  The firmware brings both controllers and link training up
 * before ExitBootServices, so user space only needs the ECAM window (PCI
 * config space) plus the outbound apertures that decode the assigned BARs.
 * Ranges come from sc8280xp-radxa-dragon-q8b.dts (&pcie2a / &pcie3b).
 */
#define Q8B_PCIE2A_ECAM_PHY      0x500000000ULL
#define Q8B_PCIE2A_ECAM_SIZE     0x10000000ULL
#define Q8B_PCIE2A_MEM32_PHY     0x510000000ULL
#define Q8B_PCIE2A_MEM32_SIZE    0x10000000ULL
#define Q8B_PCIE2A_MEM64_PHY     0x520000000ULL
#define Q8B_PCIE2A_MEM64_SIZE    0x0e0000000ULL

#define Q8B_PCIE3B_ECAM_PHY      0x440000000ULL
#define Q8B_PCIE3B_ECAM_SIZE     0x10000000ULL
#define Q8B_PCIE3B_MEM32_PHY     0x450000000ULL
#define Q8B_PCIE3B_MEM32_SIZE    0x10000000ULL
#define Q8B_PCIE3B_MEM64_PHY     0x460000000ULL
#define Q8B_PCIE3B_MEM64_SIZE    0x20000000ULL

/*
 * bootinfo hand-off (stub -> kernel), resident at Q8B_BOOTINFO_PHYS.
 * Keep the layout in sync with boot/uefi/main.c.
 */
#define Q8B_BOOTINFO_MAGIC   0x42385145u   /* "EQ8B" little-endian */
#define Q8B_BOOTINFO_VERSION 1

typedef struct {
    uint32_t type;
    uint32_t pad;
    uint64_t phys_start;
    uint64_t virt_start;
    uint64_t pages;
    uint64_t attr;
} q8b_efi_memdesc_t;

#define Q8B_MEM_USABLE(t) \
    ((t) == 1 || (t) == 2 || (t) == 3 || (t) == 4 || \
     (t) == 7 || (t) == 13)

#define Q8B_MAX_RESV 32

typedef struct {
    uint64_t base;
    uint64_t size;
} q8b_resv_t;

typedef struct {
    uint32_t magic;
    uint32_t version;

    uint64_t ram_base;
    uint64_t ram_size;

    uint64_t fb_addr;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_pitch;
    uint32_t fb_bpp;
    uint32_t fb_format;        /* UEFI GOP PixelFormat (0=RGB,1=BGR,2=BitMask) */
    uint32_t _pad_fb;

    uint64_t rd_base;          /* ROOTFS.IMG copied into RAM (0 = none) */
    uint64_t rd_size;

    uint64_t kernel_phys;
    uint64_t kernel_size;

    uint64_t memmap_addr;
    uint32_t memmap_count;
    uint32_t memmap_dsize;

    uint32_t resv_count;
    uint32_t _pad0;
    q8b_resv_t resv[Q8B_MAX_RESV];

    uint64_t dtb_addr;
    uint64_t dtb_size;
} q8b_bootinfo_t;

/* low-level debug console (bsp/dbg.c) */
extern void q8b_dbg_putc(char c);
extern void q8b_dbg_puts(const char* s);
extern void q8b_dbg_hex(uint64_t v);
extern void q8b_dbg_dec(uint64_t v);

/* GICv3 (bsp/gic_v3.c) */
extern void gic_v3_init(void);
extern void gic_v3_core_init(uint32_t core);
extern void gic_v3_irq_enable(int core, int irq);
extern void gic_v3_irq_disable(int core, int irq);
extern uint32_t gic_v3_get_irq(void);
extern void gic_v3_eoi(uint32_t irq);
extern void gic_v3_send_sgi(int core, int irq);

extern q8b_bootinfo_t* q8b_bootinfo(void);
extern uint64_t q8b_ramdisk_base;
extern uint64_t q8b_ramdisk_size;
extern uint64_t q8b_fb_base;
extern uint64_t q8b_fb_size;

#endif
