#ifndef BCM2712_MMIO_H
#define BCM2712_MMIO_H

/*
 * BCM2712 (Raspberry Pi 5) MMIO physical address windows.
 *
 * The Pi 5 has three distinct peripheral windows:
 *   1. Main window: 64 MB at 0x10_7C000000  (UART, Mailbox, GIC)
 *   2. EMMC window:  2 MB at 0x10_00E00000  (SDHCI host controller)
 *   3. RP1 window:  32 MB at 0x1F_00000000  (GPIO, SPI, I2C, etc.)
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

#define PI5_RP1_PHY         0x1F00000000ULL
#define PI5_RP1_WIN_OFF     0x08000000
#define PI5_RP1_WIN_SIZE    (32 * 1024 * 1024)

#endif
