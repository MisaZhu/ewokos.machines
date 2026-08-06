#ifndef HW_ARCH_H
#define HW_ARCH_H

#include <stdint.h>
#include <mm/mmu.h>

/*
 * BCM2712 (Raspberry Pi 5) memory map.
 *
 * The main peripheral window lives at 0x10_7C000000 (36bit physical space).
 * Boot page tables map this canonical address (uint64_t capable).
 * The final kernel VM is limited to 32bit physical (ewokos_addr_t),
 * so the MMIO window is mapped there only if the 32bit alias at 0x7C000000
 * is enabled in the BCM2712 address decoder.
 */
#define PI5_MMIO_PHY        0x107C000000ULL
#define PI5_MMIO_SIZE       (64*MB)

/* offsets inside the peripheral window */
#define PI5_UART0_OFF       0x01001000  /* PL011, console on GPIO14/15   */
#define PI5_MAILBOX_OFF     0x00013880  /* legacy VPU property mailbox   */
#define PI5_GICD_OFF        0x03FF9000  /* GIC-400 distributor           */
#define PI5_GICC_OFF        0x03FFA000  /* GIC-400 cpu interface         */

/*
 * Extra MMIO virtual windows, outside the main mmio.size window but still
 * inside MMIO_MAX_SIZE (128MB). They are mapped by the boot page tables
 * (bsp/start.c) and by arch_vm() for the final kernel VM.
 */
#define PI5_EMMC_WIN_OFF    0x04000000
#define PI5_EMMC_PHY_WIN    0x1000E00000UL   /* 2MB aligned window        */
#define PI5_EMMC_OFF        0x001FF000      /* SD host @ 0x1000FFF000    */
#define PI5_EMMC_BASE       (MMIO_BASE + PI5_EMMC_WIN_OFF + PI5_EMMC_OFF)

/* BCM2712 PCIe2 root complex used by the onboard RP1 southbridge. */
#define PI5_PCIE2_WIN_OFF   0x04400000
#define PI5_PCIE2_PHY       0x1000120000UL
#define PI5_PCIE2_SIZE      (64*KB)
#define PI5_RESET_PAGE_PHY  0x1001504000UL
#define PI5_RESCAL_PAGE_PHY 0x1000119000UL
#define PI5_RESET_PAGE_SIZE PAGE_SIZE

/*
 * RP1 has to stay strictly inside MMIO_MAX_SIZE. At offset 0x08000000 it sat
 * at exactly MMIO_BASE + MMIO_MAX_SIZE, which is DMA_V_BASE: its 32MB device
 * window then covered the whole 32MB DMA window that every process shares, so
 * a dma_alloc() and an RP1 register access aliased the same virtual pages.
 * 0x06000000 keeps the window inside MMIO_MAX_SIZE and clear of the EMMC
 * window at 0x04000000.
 *
 * Base and size both come from the official device tree. RP1 hangs off pcie2,
 * whose outbound window is
 *   ranges = <0x02000000 0x00 0x00000000  0x1f 0x00000000  0x0 0xfffffffc>;
 * i.e. PCIe 32bit memory space starts at cpu physical 0x1F_00000000, and RP1
 * maps its own peripherals 1:1 from the bottom of it:
 *   ranges = <0xc0 0x40000000  0x02000000 0x00 0x00000000  0x00 0x00410000>;
 * so the whole RP1 register file is 0x410000, not the 32MB mapped before.
 * Rounded up to the 2MB block granularity of the boot page tables (start.c
 * shifts the length by PDE_SHIFT, so a non multiple of 2MB maps nothing).
 *
 * The window only decodes after the firmware has trained the PCIe link.
 */
#define PI5_RP1_WIN_OFF     0x06000000
#define PI5_RP1_PHY         0x1F00000000UL   /* RP1 southbridge           */
#define PI5_RP1_REGS_SIZE   0x00410000       /* rp1 ranges, exact         */
#define PI5_RP1_SIZE        (6*MB)           /* 2MB block aligned         */

/*
 * Firmware framebuffer reserve.
 *
 * The official bcm2712.dtsi describes the VideoCore view of RAM as
 *   dma-ranges = <0xc0000000 0x00 0x00000000 0x40000000>;
 * that is the only RAM alias the VPU has, and it covers ARM physical
 * 0 .. 1GB only.  A mailbox allocated framebuffer therefore always lands
 * below 0x40000000, never at the top of RAM.
 *
 * Where exactly inside that 1GB the firmware puts the scan-out buffer depends
 * on the config.txt memory split. The tail of the window is kept out of the
 * kernel heap and check_mem_map_arch() only lets a scan-out be mapped from
 * there: accepting the whole 1GB instead means a firmware buffer that landed
 * elsewhere gets mapped on top of live heap pages, and the display then shows
 * whatever the allocator is doing.
 *
 * The driver does not depend on this reserve for the normal case; it hands the
 * firmware a buffer out of the sys_dma pool (which is carved out before the
 * heap exists), exactly like linux drivers/video/fbdev/bcm2708_fb.c does.
 */
#define PI5_VC_RAM_TOP      0x40000000UL              /* VPU reachable RAM */
#define PI5_FB_SIZE         (64*MB)
#define PI5_FB_LOW_BASE     (PI5_VC_RAM_TOP - PI5_FB_SIZE)
#define PI5_FB_LOW_TOP      PI5_VC_RAM_TOP

#endif
