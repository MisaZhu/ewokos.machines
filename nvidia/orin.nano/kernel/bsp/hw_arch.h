#ifndef ORIN_HW_ARCH_H
#define ORIN_HW_ARCH_H

#include <stdint.h>
#include <mm/mmu.h>

/*
 * NVIDIA Jetson Orin Nano / Tegra234 (T234) hardware map.
 *
 * Boot chain: MB1/MB2 -> TF-A BL31 (EL3, PSCI) -> NVIDIA EDK2 UEFI (EL2)
 * -> BOOTAA64.EFI (our stub) -> kernel.bin copied to ORIN_KERNEL_PHYS.
 *
 * Address sources: upstream Linux arch/arm64/boot/dts/nvidia/tegra234.dtsi
 * (gic@f400000, serial@c280000), tegra234-p3768-0000+p3767.dtsi, and the
 * hvisor/wolfBoot Orin bring-ups (UARTC earlycon uart8250,mmio32,0xc280000).
 */

#define ORIN_RAM_BASE        0x80000000ULL
#define ORIN_KERNEL_PHYS     0x90080000ULL   /* RAM base + 256MB */
#define ORIN_BOOTINFO_PHYS   0x90070000ULL   /* 64KB reserved below kernel */
#define ORIN_BOOTINFO_SIZE   0x10000ULL

/* Early boot maps a 64MB window around the kernel. */
#define ORIN_EARLY_WIN_BASE  0x90000000ULL
#define ORIN_EARLY_WIN_SIZE  (64*MB)
#define ORIN_EARLY_WIN_VOFF  (ORIN_EARLY_WIN_BASE - ORIN_RAM_BASE)

/* MMIO window: the shared kernel caps _sys_info.mmio.size at MMIO_MAX_SIZE
 * (128MB), and UARTC sits at 0x0c280000 (~194MB), so a phy-0 window cannot
 * reach it.  Anchor the window on the AON cluster instead; it only needs to
 * cover the debug UART for this bring-up port. */
#define ORIN_MMIO_PHY        0x0c000000ULL
#define ORIN_MMIO_SIZE       (4*MB)

/* Debug UART: UARTC (AON), 16550-compatible with 32-bit spaced registers
 * (reg-shift=2).  Firmware (SPE/TCU + UEFI) leaves it clocked, pinmuxed
 * and programmed for 115200 8N1; the kernel only drives TX/RX.
 * On the Orin Nano devkit it is wired to the J14 debug header (SODIMM
 * 236/238, UART2_TXD/RXD), NOT the 40-pin header (that one is UARTA). */
#define ORIN_UARTC_PHY       0x0c280000ULL
#define ORIN_UARTC_OFF       0x00280000     /* offset in ORIN_MMIO_PHY window */
#define ORIN_UART_BAUD       115200

/* GICv3: distributor + one redistributor region (2MB, 0x20000 stride).
 * Redistributors are matched to cores by MPIDR affinity, not by index.
 * Dedicated virtual window above the AON window, below MMIO_MAX_SIZE. */
#define ORIN_GIC_WIN_PHY     0x0f400000ULL
#define ORIN_GIC_WIN_SIZE    (4*MB)
#define ORIN_GIC_WIN_OFF     0x04000000     /* virtual offset from MMIO_BASE */

#ifdef ORIN_WATCHDOG_TEST
/* TEST-only SBSA watchdog boot mappings (virtual offsets from MMIO_BASE).
 * Armed early in arch_kernel_entry_early so a hang anywhere after the kernel
 * starts resets the board.  Removable: build kernel without WATCHDOG=1. */
#define ORIN_WDT_CTRL_WIN_OFF    0x05000000
#define ORIN_WDT_REFRESH_WIN_OFF 0x05010000
#define ORIN_WDT_BOOT_TIMEOUT    60u
#endif
#define ORIN_GICD_PHY        0x0f400000ULL
#define ORIN_GICR_PHY        0x0f440000ULL
#define ORIN_GICR_SIZE       0x00200000ULL
#define ORIN_GICR_STRIDE     0x20000ULL

#define ORIN_GIC_TIMER_IRQ   27             /* virtual timer PPI (CNTV) */
#define ORIN_GIC_IPI_IRQ     0              /* SGI0 */

/* Orin Nano: 6 Cortex-A78AE cores, two clusters (4+2).  Tegra234 MPIDR
 * affinity is Aff2=cluster, Aff1=core-in-cluster, Aff0=0.  Values from
 * tegra234.dtsi cpu nodes as pruned for p3767, cross-checked against the
 * hvisor Orin Nano board config (BOARD_MPIDR_MAPPINGS) and its zone0.dts:
 * cluster0 (Aff2=0) Aff1=0..3, cluster1 (Aff2=1) Aff1=2..3.  The generic
 * Aff0|Aff1 core-id merge drops Aff2 and collides, so the BSP resolves
 * logical ids through this table.  Logical core id == table index. */
#define ORIN_CORES           6

/* 8GB module (p3767-0003/-0005).  Firmware-owned holes are filtered
 * through the final UEFI memory map before pages are published. */
#define ORIN_USABLE_LIMIT    (8ULL*GB)

/* Firmware framebuffer is mapped by user space (SYS_MEM_MAP). */
#define ORIN_FB_MAX_SIZE     (64*MB)

/*
 * PCIe C8 (RTL8168 ethernet) hardware map lives in the shared machine header
 * include/orin_net.h so the kernel whitelist and the user-space PCIe/MAC
 * drivers use identical addresses.
 */
#include "orin_net.h"

/*
 * bootinfo hand-off (stub -> kernel), resident at ORIN_BOOTINFO_PHYS.
 * Layout mirrors the Q8B hand-off so the shared userland (init/sdfsd via
 * q8b_platform.h) consumes it unchanged.  Keep in sync with ../efi/main.c.
 */
#define ORIN_BOOTINFO_MAGIC   0x4e524f45u   /* "EORN" little-endian */
#define ORIN_BOOTINFO_VERSION 1

typedef struct {
    uint32_t type;
    uint32_t pad;
    uint64_t phys_start;
    uint64_t virt_start;
    uint64_t pages;
    uint64_t attr;
} orin_efi_memdesc_t;

#define ORIN_MEM_USABLE(t) \
    ((t) == 1 || (t) == 2 || (t) == 3 || (t) == 4 || \
     (t) == 7 || (t) == 13)

#define ORIN_MAX_RESV 32

typedef struct {
    uint64_t base;
    uint64_t size;
} orin_resv_t;

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

    uint64_t rd_base;          /* INITRAMFS.IMG copied into RAM */
    uint64_t rd_size;
    uint32_t rd_flags;         /* Q8B_RD_FLAG_* (shared hand-off layout) */
    uint32_t boot_source;      /* Q8B_BOOT_SOURCE_* */

    uint64_t kernel_phys;
    uint64_t kernel_size;

    uint64_t memmap_addr;
    uint32_t memmap_count;
    uint32_t memmap_dsize;

    uint32_t resv_count;
    uint32_t serial_flags;   /* bit0: UARTC usable (loader LSR probe) */
    orin_resv_t resv[ORIN_MAX_RESV];

    uint64_t dtb_addr;
    uint64_t dtb_size;

    /* Device model from the firmware DTB /model. */
    char model[32];
} orin_bootinfo_t;

/* MPIDR affinity per logical core (index == logical core id). */
extern const uint64_t orin_core_mpidr[ORIN_CORES];

/* Logical core id of the calling CPU (MPIDR table lookup). */
uint32_t orin_core_id(void);

/* low-level debug console (bsp/dbg.c) */
extern void orin_dbg_putc(char c);
extern void orin_dbg_write(const char* s, uint32_t size);
extern void orin_dbg_puts(const char* s);
extern void orin_dbg_hex(uint64_t v);
extern void orin_dbg_dec(uint64_t v);

/* GICv3 (bsp/gic_v3.c) */
extern void gic_v3_init(void);
extern void gic_v3_core_init(uint32_t core);
extern void gic_v3_irq_enable(int core, int irq);
extern void gic_v3_irq_disable(int core, int irq);
extern uint32_t gic_v3_get_irq(void);
extern void gic_v3_eoi(uint32_t irq);
extern void gic_v3_send_sgi(int core, int irq);

extern int orin_serial_on;   /* bootinfo serial_flags bit0, set before any kout */
extern orin_bootinfo_t* orin_bootinfo(void);
extern uint64_t orin_ramdisk_base;
extern uint64_t orin_ramdisk_size;
extern uint32_t orin_ramdisk_flags;
extern uint64_t orin_fb_base;
extern uint64_t orin_fb_size;

#endif