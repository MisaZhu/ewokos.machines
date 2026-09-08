#ifndef BCM2712_MMIO_H
#define BCM2712_MMIO_H

/*
 * BCM2712 (Raspberry Pi 5) MMIO physical address windows.
 *
 * The Pi 5 has four distinct peripheral windows:
 *   1. Main window: 64 MB at 0x10_7C000000  (UART, Mailbox, GIC)
 *   2. EMMC window:  4 MB at 0x10_00E00000  (SDHCI hosts: SD card + WiFi SDIO)
 *   3. RP1 window:   6 MB at 0x1F_00000000  (GPIO, SPI, I2C, xHCI, etc.)
 *   4. USB2 window: 64 KB at 0x10_00480000  (SoC DWC2 host, CM5's USB 2.0)
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
/*
 * 4MB: carries both SDHCI hosts of bcm2712.dtsi, sdio1 (SD card) at
 * 0x1000FFF000 and sdio2 (WLAN) at 0x1001100000. Must match
 * PI5_EMMC_WIN_SIZE in machines/raspi5/kernel/bsp/hw_arch.h.
 */
#define PI5_EMMC_WIN_SIZE   (4 * 1024 * 1024)
#define PI5_EMMC_OFF        0x001FF000      /* SD card host @ 0x1000FFF000 */
#define PI5_WLAN_SDIO_OFF   0x00300000      /* WiFi SDIO host @ 0x1001100000 */
#define PI5_WLAN_SDIO_CFG_OFF 0x00300400    /* WiFi host cfg regs (+0x400) */

/* Offsets inside the main window (relative to _mmio_base) used by the
 * WiFi bring-up: BCM2712 SoC pinctrl and the "gio" brcmstb-gpio bank
 * that drives WL_REG_ON (bcm2712.dtsi). */
#define PI5_PINCTRL_OFF     0x01504100      /* pinctrl@7d504100 */
#define PI5_GIO_OFF         0x01508500      /* gio: gpio@7d508500 */

/*
 * Must match PI5_RP1_WIN_OFF/PI5_RP1_SIZE in machines/raspi5/kernel/bsp/hw_arch.h:
 * the kernel maps RP1 there, userspace only reuses the offset. 0x08000000 was
 * exactly MMIO_BASE + MMIO_MAX_SIZE, i.e. DMA_V_BASE, so the RP1 window and
 * the shared DMA window aliased the same virtual pages.
 *
 * The size is what rp1.dtsi declares for the whole register file,
 *   ranges = <0xc0 0x40000000  0x02000000 0x00 0x00000000  0x00 0x00410000>;
 * rounded up to the 2MB granularity the kernel boot page tables use. It also
 * has to stay <= PI5_RP1_SIZE, or check_mem_map_arch() refuses the mapping.
 */
#define PI5_RP1_PHY         0x1F00000000ULL

/*
 * BCM2712 PCIe1 root complex — the EXTERNAL PCIe connector where an NVMe HAT
 * is attached (bcm2712.dtsi: pcie1@110000, reg 0x10_00110000).
 *
 * DISTINCT from PCIe2 (0x10_00120000), which is the onboard RP1 southbridge.
 * The two are independent controllers with their own DBI register blocks and
 * their own PERST/bridge reset lines; an NVMe on the external connector
 * appears on pcie1's bus, never on pcie2's.
 *
 * Outbound memory window (dtsi ranges):
 *   <0x02000000 0x00 0x80000000  0x1b 0x80000000  0x00 0x80000000>
 * i.e. CPU physical 0x1b_80000000 .. 0x1b_ffffffff maps to PCIe addr 0x0.
 * An NVMe BAR0 lands somewhere in this 2 GB window; nvme.c maps it via
 * SYS_MEM_MAP, so check_mem_map_arch() must whitelist the whole window.
 *
 * Virtual offset 0x04600000 is chosen inside MMIO_MAX_SIZE and clear of the
 * EMMC window (0x04000000), pcie2 host window (0x04400000), the RP1 ctrl
 * window (0x04500000) and the RP1 register window (0x06000000).
 */
#define PI5_PCIE1_PHY          0x1000110000ULL
#define PI5_PCIE1_WIN_OFF      0x04600000
#define PI5_PCIE1_WIN_SIZE     (64 * 1024)
#define PI5_PCIE1_BAR0_WIN_OFF (PI5_PCIE1_WIN_OFF + 0x00010000) /* 64KB past host */

/* CPU-physical outbound memory window for pcie1 (dtsi ranges). */
#define PI5_PCIE1_MEM_WIN_PHY  0x1b80000000ULL
#define PI5_PCIE1_MEM_WIN_SIZE 0x80000000ULL           /* 2 GB */

/*
 * BCM2712 reset controller (brcmstb-reset) at 0x10_01504318, 0x30 bytes.
 * Per-bank stride 0x18; each bank = 3 regs (set/clear/status) + padding.
 * Bank = id/32, bit = id%32.  pcie1 DT resets = <&bcm_reset 7 swinit>,
 * <&bcm_reset 43 bridge>, <&pcie_rescal>.  pcie2 uses ids 32/44 — this is
 * why rp1.c's "bank 1 bit 12" (id 44) brings up RP1, not the NVMe.
 */
#define PI5_RESET_CTRL_PHY     0x10001504318ULL
#define PI5_RESET_CTRL_WIN_OFF 0x04620000
#define PI5_RESET_CTRL_WIN_SIZE (4 * 1024)
#define PI5_RESCAL_PAGE_PHY     0x1000119000ULL
#define PI5_RESCAL_WIN_OFF     0x04630000
#define PI5_RESCAL_WIN_SIZE    (4 * 1024)

#define PI5_PCIE2_PHY       0x1000120000ULL
#define PI5_PCIE2_WIN_OFF   0x04400000
#define PI5_PCIE2_WIN_SIZE  (64 * 1024)
#define PI5_RESET_PAGE_PHY  0x1001504000ULL

/*
 * RP1 control windows are sub-page MMIO regions. Userspace must map them
 * according to sys_info.page_size, so only reserve the first slot here and let
 * rp1.c place each control page on a dedicated runtime-sized page.
 */
#define PI5_RP1_CTRL_WIN_OFF 0x04500000

/*
 * BCM2712 SoC USB 2.0 host (Synopsys DWC2, "brcm,bcm2835-usb"): 64 KB at
 * 0x10_00480000, a separate window from the main peripheral block and from
 * RP1. CM5 boards such as the uConsole hang their onboard hub (keyboard +
 * trackball) off this controller, so usbhostd drives it alongside the RP1
 * xHCI pair.
 *
 * Must match PI5_USB_DWC2_WIN_OFF/PI5_USB_DWC2_PHY/PI5_USB_DWC2_SIZE in
 * machines/raspi5/kernel/bsp/hw_arch.h: the kernel maps nothing here, it only
 * whitelists the physical range in check_mem_map_arch() so the SYS_MEM_MAP
 * below is accepted.
 */
#define PI5_USB_DWC2_PHY      0x1000480000ULL
#define PI5_USB_DWC2_WIN_OFF  0x04700000
#define PI5_USB_DWC2_WIN_SIZE (64 * 1024)

/* Must match PI5_RP1_WIN_OFF/PI5_RP1_SIZE in kernel/bsp/hw_arch.h. */
#define PI5_RP1_WIN_OFF     0x06000000
#define PI5_RP1_WIN_SIZE    (6 * 1024 * 1024)

#endif
