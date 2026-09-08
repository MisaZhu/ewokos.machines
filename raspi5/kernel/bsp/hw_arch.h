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

/* VideoCore VII (V3D) block: hub at 0x1002000000, core0 at +0x8000,
   SMS at +0x30800.  Outside the main MMIO window; the g2d driver maps
   this window through SYS_MEM_MAP to drive CSD dispatch. */
#define PI5_V3D_PHY        0x1002000000ULL
#define PI5_V3D_SIZE       (4*MB)

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
#define PI5_EMMC_PHY_WIN    0x1000E00000UL   /* 4MB aligned window        */
/*
 * One window carries both SDHCI hosts of bcm2712.dtsi:
 *   sdio1 (SD card):  mmc@fff000        -> 0x1000FFF000
 *   sdio2 (WLAN):     mmc@1100000       -> 0x1001100000, +0x400 cfg regs
 * 4MB covers 0x1000E00000 .. 0x1001200000, still clear of the PCIe2
 * window at 0x04400000.
 */
#define PI5_EMMC_WIN_SIZE   (4*MB)
#define PI5_EMMC_OFF        0x001FF000      /* SD host @ 0x1000FFF000    */
#define PI5_EMMC_BASE       (MMIO_BASE + PI5_EMMC_WIN_OFF + PI5_EMMC_OFF)
#define PI5_WLAN_SDIO_OFF   0x00300000      /* WiFi host @ 0x1001100000  */
#define PI5_WLAN_SDIO_CFG_OFF 0x00300400    /* WiFi host cfg regs        */

/*
 * BCM2712 PCIe1 root complex — the EXTERNAL PCIe connector where an NVMe
 * HAT is attached (bcm2712.dtsi: pcie1@110000, reg 0x10_00110000).
 *
 * DISTINCT from PCIe2 below: pcie2 is the onboard RP1 southbridge and has
 * no external downstream port.  An NVMe on the external connector appears
 * on pcie1's bus, never on pcie2's — the userspace driver must target
 * pcie1 (the previous code only touched pcie2 and never found the NVMe).
 *
 * Outbound memory window (dtsi ranges):
 *   <0x02000000 0x00 0x80000000  0x1b 0x80000000  0x00 0x80000000>
 * CPU physical 0x1b_80000000 .. 0x1b_ffffffff maps to PCIe addr 0x0.
 * An NVMe BAR0 lands here; check_mem_map_arch() must whitelist the 2 GB
 * window or the driver's SYS_MEM_MAP of BAR0 is refused and reads abort.
 *
 * Virtual offset 0x04600000 is inside MMIO_MAX_SIZE and clear of EMMC
 * (0x04000000), pcie2 host window (0x04400000) and RP1 (0x06000000).
 */
#define PI5_PCIE1_WIN_OFF      0x04600000
#define PI5_PCIE1_PHY          0x1000110000UL
#define PI5_PCIE1_SIZE         (64*KB)
#define PI5_PCIE1_MEM_WIN_PHY  0x1b80000000UL
#define PI5_PCIE1_MEM_WIN_SIZE 0x80000000UL    /* 2 GB */

/* BCM2712 PCIe2 root complex used by the onboard RP1 southbridge. */
#define PI5_PCIE2_WIN_OFF   0x04400000
#define PI5_PCIE2_PHY       0x1000120000UL
#define PI5_PCIE2_SIZE      (64*KB)
#define PI5_RESET_PAGE_PHY  0x1001504000UL
#define PI5_RESCAL_PAGE_PHY 0x1000119000UL
#define PI5_RESET_PAGE_SIZE PAGE_SIZE
#define PI5_RESCAL_PAGE_SIZE PAGE_SIZE

/*
 * BCM2712 SoC USB 2.0 host (Synopsys DWC2, same IP as bcm283x).
 *
 * bcm2712.dtsi declares usb@480000, compatible "brcm,bcm2835-usb", at
 * 0x10_00480000 with a 64 KB register file, interrupts GIC_SPI 73 and a
 * usb-nop-xceiv PHY (nothing to program); bcm2712-rpi.dtsi binds it to a
 * firmware power domain, so it is only alive after a mailbox power-on.
 *
 * This is NOT the RP1 xHCI pair. CM5 exposes this SoC controller as its
 * "built-in USB 2.0 hub" and a board such as the uConsole wires the onboard
 * GL850 hub (keyboard + trackball) to it — its config.txt carries
 * dtoverlay=dwc2,dr_mode=host under [all], which is what activates those
 * ports for pi5 as well as pi4. usbhostd therefore has to drive it too, and
 * maps the window through SYS_MEM_MAP: without this whitelist entry the
 * mapping is refused and the first register read aborts.
 *
 * Virtual offset 0x04700000 sits inside MMIO_MAX_SIZE and clear of EMMC
 * (0x04000000), pcie2 (0x04400000), rp1 ctrl (0x04500000), pcie1 plus its
 * BAR0 sub-window (0x04600000), reset/rescal (0x04620000/0x04630000) and
 * RP1 (0x06000000).
 */
#define PI5_USB_DWC2_WIN_OFF 0x04700000
#define PI5_USB_DWC2_PHY     0x1000480000UL
#define PI5_USB_DWC2_SIZE    (64*KB)

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
