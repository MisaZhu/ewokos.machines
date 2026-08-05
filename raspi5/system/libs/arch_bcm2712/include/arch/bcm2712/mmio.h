#ifndef BCM2712_MMIO_H
#define BCM2712_MMIO_H

/*
 * BCM2712 (Raspberry Pi 5) MMIO physical address windows.
 *
 * The Pi 5 has three distinct peripheral windows:
 *   1. Main window: 64 MB at 0x10_7C000000  (UART, Mailbox, GIC)
 *   2. EMMC window:  2 MB at 0x10_00E00000  (SDHCI host controller)
 *   3. RP1 window:   6 MB at 0x1F_00000000  (GPIO, SPI, I2C, etc.)
 *
 * ewokos_addr_t is uint64_t on aarch64, so full 64-bit physical
 * addresses are passed to syscall3(SYS_MEM_MAP, ...).
 *
 * Virtual offsets are relative to _mmio_base (i.e. MMIO_BASE on the
 * kernel side, or the value returned by syscall3).
 */

#define PI5_MMIO_PHY        0x107C000000ULL
#define PI5_MMIO_SIZE       (64 * 1024 * 1024)

#define PI5_EMMC_PHY_WIN    0x1000E00000ULL
#define PI5_EMMC_WIN_OFF    0x04000000
#define PI5_EMMC_WIN_SIZE   (2 * 1024 * 1024)

/*
 * Must match PI5_RP1_WIN_OFF/PI5_RP1_SIZE in machines/raspi5/kernel/bsp/hw_arch.h:
 * the kernel maps RP1 there, userspace only reuses the offset. 0x08000000 was
 * exactly MMIO_BASE + MMIO_MAX_SIZE, i.e. DMA_V_BASE, so the RP1 window and
 * the shared DMA window aliased the same virtual pages.
 *
 * The size is what rp1.dtsi declares for the whole register file,
 *   ranges = <0xc0 0x40000000  0x02000000 0x00 0x00000000  0x00 0x00410000>;
 * rounded up to the 2MB granularity the kernel boot page tables use.
 */
#define PI5_RP1_PHY         0x1F00000000ULL
#define PI5_RP1_WIN_OFF     0x06000000
#define PI5_RP1_WIN_SIZE    (6 * 1024 * 1024)

#endif
