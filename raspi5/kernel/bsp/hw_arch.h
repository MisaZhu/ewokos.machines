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

#define PI5_RP1_WIN_OFF     0x08000000
#define PI5_RP1_PHY         0x1F00000000UL   /* RP1 southbridge           */
#define PI5_RP1_SIZE        (32*MB)

/* framebuffer reserve at top of physical RAM (mailbox fb alloc) */
#define PI5_FB_SIZE         (8*MB)

#endif
