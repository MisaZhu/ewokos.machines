#ifndef ORIN_NET_H
#define ORIN_NET_H

/*
 * Orin Nano networking hardware map (shared by the kernel BSP whitelist and
 * the user-space PCIe/Ethernet drivers).
 *
 * The devkit RJ45 is a Realtek RTL8168 gigabit NIC on PCIe C8
 * (pcie@140a0000, linux,pci-domain 8) -- NOT the SoC MGBE (AGX Orin only).
 * Bring-up inherits the UEFI PCIe state (PXE enumerates C8 + RTL8168); the
 * driver reads config space through ECAM and drives the MAC, with no BPMP
 * or root-complex bring-up.  Addresses from upstream tegra234.dtsi.
 */

#define ORIN_PCIE_C8_APPL_PHY   0x140a0000ULL   /* appl regs (128K) */
#define ORIN_PCIE_C8_CFG_PHY    0x2a000000ULL   /* config viewport (256K) */
#define ORIN_PCIE_C8_ATU_PHY    0x2a040000ULL   /* iATU/DMA regs (256K) */
#define ORIN_PCIE_C8_DBI_PHY    0x2a080000ULL   /* DBI regs (256K) */
#define ORIN_PCIE_C8_ECAM_PHY   0x3530000000ULL /* ECAM (256MB) */
/* Non-prefetchable MMIO range: PCI 0x40000000 -> CPU 0x3528000000 (128MB).
 * RTL8168 BAR0 lands here; BAR_phys = BAR_pci + (MMIO_PHY - MMIO_PCI). */
#define ORIN_PCIE_C8_MMIO_PCI   0x40000000ULL
#define ORIN_PCIE_C8_MMIO_PHY   0x3528000000ULL
#define ORIN_PCIE_C8_MMIO_SIZE  0x08000000ULL
/* ECAM sub-range mapped for enumeration (bus 0..15 = 16MB). */
#define ORIN_PCIE_C8_ECAM_MAP_SIZE 0x01000000ULL

#define ORIN_RTL8168_VENDOR     0x10ec
#define ORIN_RTL8168_DEVICE     0x8168
#define ORIN_RTL8168_DEVICE2    0x8161   /* RTL8111/8168 family alias */

/*
 * BPMP IVC (clock / reset / powergate / UPHY control).  Tegra234 talks to
 * the BPMP firmware through HSP_TOP0 doorbells + a shared-SRAM IVC ring.
 * Addresses from tegra234.dtsi (hsp_top0, sysram, bpmp) and the upstream
 * tegra-hsp / bpmp-tegra186 / ivc drivers.  Used by the cold-own PCIe RC
 * bring-up; the inherit-UEFI Ethernet path does not need it.
 */
#define ORIN_HSP_TOP0_PHY       0x03c00000ULL
#define ORIN_HSP_TOP0_SIZE      0x000a0000ULL
/* Doorbell block base = (1 + num_sm/2 + num_ss + num_as)*64K; top0 has 8 SM
 * -> 0x50000, stride 0x100.  CCPLEX idx1 -> 0x50100, BPMP idx3 -> 0x50300. */
#define ORIN_HSP_DB_BASE        0x50000u
#define ORIN_HSP_DB_CCPLEX      (ORIN_HSP_DB_BASE + 1u * 0x100u) /* 0x50100 */
#define ORIN_HSP_DB_BPMP        (ORIN_HSP_DB_BASE + 3u * 0x100u) /* 0x50300 */
#define ORIN_HSP_DB_TRIGGER     0x00u
#define ORIN_HSP_DB_ENABLE      0x04u
#define ORIN_HSP_DB_PENDING     0x0cu
#define ORIN_HSP_MASTER_BPMP    19u   /* CCPLEX PENDING/ENABLE bit for BPMP */
#define ORIN_HSP_INT_DIMENSIONING 0x380u

/* Tegra234 NVDisplay (NISO display@13800000).  The register map is SPARSE:
 * FE +0 reads cleanly but +4 is a register HOLE (decode SError), NOT a bus
 * firewall -- known-valid FE offsets (0x2200 CMGR, 0x30000 SW_SYS_CAP) read
 * fine with the panel already active.  Span 0x700000 covers the FE block plus
 * the NV_PDISP head/window region (0x610000..0x6f1fff).  Window assembly block
 * = 0x690000 + win*0x400 (offsets RE'd from nvgop-chips-platform.efi).  Only
 * firmware-validated offsets may be probed -- blind reads of holes = SError. */
#define ORIN_NVDISPLAY_PHY      0x13800000ULL
#define ORIN_NVDISPLAY_SIZE     0x00700000ULL
/* DCE (Display Control Engine) aperture, dt-bindings: dce@d800000 0x800000.
 * T234 modeset is DCE-mediated; probe to confirm the closed GOP register base. */
#define ORIN_DCE_PHY            0x0d800000ULL
#define ORIN_DCE_SIZE           0x00800000ULL
/* BPMP clock IDs (dt-bindings/clock/tegra234-clock.h). */
#define ORIN_DISP_CLK_HUB       84u   /* NVDISPLAYHUB */
#define ORIN_DISP_CLK_DISP      85u   /* DISPCLK */
#define ORIN_DISP_CLK_P0        86u   /* NVDISPLAY_P0 (head0 RG) */
#define ORIN_DISP_CLK_P1        87u   /* NVDISPLAY_P1 (head1 RG) */
#define ORIN_DISP_CLK_DSC       88u   /* DSC */
/* BPMP reset ID (dt-bindings/reset/tegra234-reset.h). */
#define ORIN_DISP_RESET         16u   /* NVDISPLAY */
/* BPMP power domain (dt-bindings/power/tegra234-powergate.h). */
#define ORIN_DISP_PG            3u    /* DISP */

/* BPMP IVC shared SRAM (sysram@40000000); map non-cacheable. */
#define ORIN_BPMP_TX_PHY        0x40070000ULL
#define ORIN_BPMP_RX_PHY        0x40071000ULL
#define ORIN_BPMP_SRAM_SIZE     0x1000u
/* IVC channel positions (frame index * 256B).  cpu_tx = tx+0x300,
 * cpu_rx = rx+0xD00.  Header 128B (tx.count@0, tx.state@4, rx.count@0x40),
 * frame @ +0x80, num_frames=1, frame_size=128. */
#define ORIN_BPMP_CPU_TX_OFF    0x000u
#define ORIN_BPMP_CPU_RX_OFF    0x000u
#define ORIN_IVC_HDR_SIZE       128u
#define ORIN_IVC_FRAME_SIZE     128u
#define ORIN_IVC_TX_COUNT       0x00u
#define ORIN_IVC_TX_STATE       0x04u
#define ORIN_IVC_RX_COUNT       0x40u
#define ORIN_IVC_STATE_ESTABLISHED 0u
#define ORIN_IVC_STATE_SYNC     1u
#define ORIN_IVC_STATE_ACK      2u

/* BPMP MRQ codes (bpmp-abi.h). */
#define ORIN_MRQ_PG             66u
#define ORIN_MRQ_RESET          20u
#define ORIN_MRQ_CLK            22u
#define ORIN_MRQ_THERMAL        27u
#define ORIN_MRQ_UPHY           69u
#define ORIN_MRQ_BWMGR_INT      83u
#define ORIN_MRQ_FLAG_DO_ACK    0x01u
#define ORIN_MRQ_FLAG_RING_DB   0x02u
/* CLK subcommands */
#define ORIN_CLK_GET_RATE       1u
#define ORIN_CLK_SET_RATE       2u
#define ORIN_CLK_ROUND_RATE     3u
#define ORIN_CLK_GET_PARENT     4u
#define ORIN_CLK_SET_PARENT     5u
#define ORIN_CLK_IS_ENABLED     6u
#define ORIN_CLK_ENABLE         7u
#define ORIN_CLK_DISABLE        8u
#define ORIN_CLK_GET_ALL_INFO   14u
/* BWMGR_INT: subcommand, floor unit, client id (bpmp_bwmgr_ids) */
#define ORIN_BWMGR_CALC_AND_SET 2u
#define ORIN_BWMGR_UNIT_KHZ     1u
#define ORIN_BWMGR_ID_GPU       5u
#define ORIN_CLK_EMC            31u   /* TEGRA234_CLK_EMC */
/* THERMAL subcommands (mrq_thermal_host_to_bpmp_cmd) */
#define ORIN_THERMAL_QUERY_ABI  0u
#define ORIN_THERMAL_GET_TEMP   1u   /* req {u32 type, u32 zone}, resp i32 mC */
#define ORIN_THERMAL_GET_NUM_ZONES 3u /* req {u32 type}, resp u32 num */
#define ORIN_THERMAL_GET_THERMTRIP 4u /* req {u32 type, u32 zone}, resp i32 mC */
/* Thermal zone IDs (dt-bindings/thermal/tegra234-bpmp-thermal.h). */
#define ORIN_THERM_ZONE_CPU     0u
#define ORIN_THERM_ZONE_GPU     1u
#define ORIN_THERM_ZONE_CV0     2u
#define ORIN_THERM_ZONE_CV1     3u
#define ORIN_THERM_ZONE_CV2     4u
#define ORIN_THERM_ZONE_SOC0    5u
#define ORIN_THERM_ZONE_SOC1    6u
#define ORIN_THERM_ZONE_SOC2    7u
#define ORIN_THERM_ZONE_TJ_MAX  8u
#define ORIN_THERM_ZONE_COUNT   9u
/* RESET subcommands */
#define ORIN_RESET_ASSERT       1u
#define ORIN_RESET_DEASSERT     2u
/* PG subcommands */
#define ORIN_PG_SET_STATE       1u
#define ORIN_PG_STATE_ON        1u
#define ORIN_PG_STATE_OFF       0u
/* UPHY subcommands */
#define ORIN_UPHY_PLL_INIT      3u
#define ORIN_UPHY_CTRL_STATE    4u
#define ORIN_UPHY_PLL_OFF       5u

/* PCIe controller BPMP resource IDs (tegra234 clock/reset/powergate dt). */
/* C1 (M.2 Key-E WiFi, pcie@14100000) */
#define ORIN_C1_CLK_CORE        221u  /* PEX0_C1_CORE */
#define ORIN_C1_CLK_MONITOR     230u
#define ORIN_C1_RESET_CORE      117u
#define ORIN_C1_RESET_APB       121u
#define ORIN_C1_PG              9u    /* PCIEX1A */
/* C8 (Ethernet, pcie@140a0000) */
#define ORIN_C8_CLK_CORE        172u  /* PEX2_C8_CORE */
#define ORIN_C8_CLK_M           245u
#define ORIN_C8_RESET_CORE      25u
#define ORIN_C8_RESET_APB       26u
#define ORIN_C8_PG              13u   /* PCIEX4CA */

/* PCIe C1 register windows (pcie@14100000). */
#define ORIN_PCIE_C1_APPL_PHY   0x14100000ULL
#define ORIN_PCIE_C1_CFG_PHY    0x30000000ULL
#define ORIN_PCIE_C1_ATU_PHY    0x30040000ULL
#define ORIN_PCIE_C1_DBI_PHY    0x30080000ULL
#define ORIN_PCIE_C1_ECAM_PHY   0x20b0000000ULL
/* C1 non-prefetchable MMIO: PCI 0x40000000 -> CPU 0x20a8000000 (128MB). */
#define ORIN_PCIE_C1_MMIO_PCI   0x40000000ULL
#define ORIN_PCIE_C1_MMIO_PHY   0x20a8000000ULL
#define ORIN_PCIE_C1_MMIO_SIZE  0x08000000ULL
/* C1 Key-E uses p2u_hsio_3, 1 lane. */
#define ORIN_C1_P2U_PHY         0x03e30000ULL
/* C8 P2U (gbe_2/3), 2 lane. */
#define ORIN_C8_P2U0_PHY        0x03f40000ULL
#define ORIN_C8_P2U1_PHY        0x03f50000ULL

/* RTL8822CE WiFi (PCIe C1): enumerable, but softmac -- needs an 802.11 stack
 * EwokOS does not have.  Recorded for the enumeration snapshot only. */
#define ORIN_RTL8822CE_VENDOR   0x10ec
#define ORIN_RTL8822CE_DEVICE   0xc822

/* ================================================================== */
/* On-board peripherals (I2C / GPIO / pinmux / SDMMC / USB).           */
/* Addresses from tegra234.dtsi + p3767/p3768 board dtsi.              */
/* ================================================================== */

/* I2C controllers (tegra194-i2c, packet mode + MST FIFO, 0x100 each). */
#define ORIN_I2C_GEN1_PHY       0x03160000ULL  /* module EEPROM 0x50, carrier 0x57 */
#define ORIN_I2C_GEN2_PHY       0x0c240000ULL  /* INA3221 power monitor 0x40 */
#define ORIN_I2C_CAM_PHY        0x03180000ULL
#define ORIN_I2C_GEN8_PHY       0x0c250000ULL
#define ORIN_I2C_REG_SIZE       0x100u
#define ORIN_EEPROM_MOD_ADDR    0x50
#define ORIN_EEPROM_CAR_ADDR    0x57
#define ORIN_INA3221_ADDR       0x40

/* GPIO controllers (tegra186-gpio): security + regs windows. */
#define ORIN_GPIO_MAIN_SEC_PHY  0x02200000ULL
#define ORIN_GPIO_MAIN_PHY      0x02210000ULL
#define ORIN_GPIO_AON_SEC_PHY   0x0c2f0000ULL
#define ORIN_GPIO_AON_PHY       0x0c2f1000ULL
#define ORIN_GPIO_REG_SIZE      0x10000u

/* Pinmux (per-pin registers). */
#define ORIN_PINMUX_MAIN_PHY    0x02430000ULL
#define ORIN_PINMUX_MAIN_SIZE   0x19100u
#define ORIN_PINMUX_AON_PHY     0x0c300000ULL

/* SDMMC1 (microSD on p3767 module). */
#define ORIN_SDMMC1_PHY         0x03400000ULL
#define ORIN_SDMMC1_SIZE        0x20000u
#define ORIN_SDMMC1_CLK         120u  /* SDMMC1 */
#define ORIN_SDMMC1_CLK_PARENT  241u  /* PLLC4_MUXED */
#define ORIN_SDMMC1_TMCLK       219u  /* SDMMC_LEGACY_TM, 12MHz */
#define ORIN_SDMMC1_RESET       82u
/* Card detect = main GPIO port G pin 7 (active low). */
#define ORIN_SD_CD_GPIO         (6u * 8u + 7u)

/* GA10B integrated GPU (gpu@17000000, nvidia,ga10b): BAR0 register
 * aperture, 16 MB.  The DT node has no iommus (dma-coherent, physical
 * addresses through the GPU's own GMMU).  The GPU power domain, clocks and
 * reset must be BPMP-enabled BEFORE any register access. */
#define ORIN_GPU_PHY            0x17000000ULL
#define ORIN_GPU_SIZE           0x01000000u

/* PWM3 (pwm@32a0000) -- devkit fan (pwm-fan, pwms=<&pwm3 0 45334>).
 * T234 uses the tegra186 PWM model: 1 channel, regs at base + hwpwm*16,
 * CSR: enable bit31, duty ticks [23:16] (depth 256), prescaler [12:0].
 * The fan clock must be BPMP-enabled BEFORE any register access
 * (unclocked peripheral MMIO -> EL3 SError on this board). */
#define ORIN_PWM3_PHY           0x032a0000ULL
#define ORIN_PWM3_SIZE          0x10000u
#define ORIN_PWM3_CLK           107u  /* TEGRA234_CLK_PWM3 */
#define ORIN_PWM3_RESET         70u   /* TEGRA234_RESET_PWM3 */
#define ORIN_CLK_PLLP_OUT0      102u  /* assigned-clock-parent in dtsi */
#define ORIN_FAN_PWM_PERIOD_NS  45334u /* ~22kHz, board dtsi */

/* USB xHCI host (usb@3610000) + padctl. */
#define ORIN_XUSB_HCD_PHY       0x03610000ULL
#define ORIN_XUSB_HCD_SIZE      0x40000u
#define ORIN_XUSB_FPCI_PHY      0x03600000ULL
#define ORIN_XUSB_BAR2_PHY      0x03650000ULL
#define ORIN_XUSB_PADCTL_PHY    0x03520000ULL
#define ORIN_XUSB_PADCTL_SIZE   0x20000u
#define ORIN_XUSB_PADCTL_AO_PHY 0x03540000ULL
#define ORIN_XUSB_PG_A          10u   /* XUSBA */
#define ORIN_XUSB_PG_C          12u   /* XUSBC */
#define ORIN_XUSB_CLK_PLLE      100u
#define ORIN_XUSB_CLK_HOST      267u  /* XUSB_CORE_HOST */
#define ORIN_XUSB_CLK_FALCON    269u
#define ORIN_XUSB_CLK_CORE_SS   268u
#define ORIN_XUSB_CLK_SS        275u
#define ORIN_XUSB_CLK_FS        272u
#define ORIN_XUSB_CLK_UTMIP_PLL 103u
#define ORIN_XUSB_CLK_M         14u
#define ORIN_XUSB_CLK_USB2_TRK  165u
#define ORIN_XUSB_RESET_PADCTL  114u

/* ================================================================== */
/* TEST-ONLY hardware watchdog (SBSA Generic Watchdog on Tegra234).    */
/* NOT for mainline; gated by the WATCHDOG_TEST build flag.  Base      */
/* addresses from the NVIDIA tegra-wdt-t234 node (watchdog@2190000):   */
/* control frame 0x2190000, refresh frame 0x2090000.  Register layout  */
/* from upstream sbsa_gwdt.c (ARM DEN0029).                            */
/* ================================================================== */
#define ORIN_WDT_CTRL_PHY       0x02190000ULL
#define ORIN_WDT_REFRESH_PHY    0x02090000ULL
#define ORIN_WDT_FRAME_SIZE     0x10000u
/* control frame */
#define ORIN_WDT_WCS            0x000   /* bit0 EN, bit1 WS0, bit2 WS1 */
#define ORIN_WDT_WOR            0x008   /* watchdog offset range (ticks) */
#define ORIN_WDT_WCV            0x010   /* 64-bit compare value */
#define ORIN_WDT_W_IIDR         0xfcc
/* refresh frame */
#define ORIN_WDT_WRR            0x000   /* write any value to refresh */
#define ORIN_WDT_WCS_EN         (1u << 0)
#define ORIN_WDT_WCS_WS0        (1u << 1)
#define ORIN_WDT_WCS_WS1        (1u << 2)
/* Boot-stage timeout programmed by the loader; the feeder takes over. */
#define ORIN_WDT_BOOT_TIMEOUT   60u
#define ORIN_WDT_RUN_TIMEOUT    60u

/* ================================================================== */
/* VIC 2D engine + host1x (Tegra234).  Addresses from mainline         */
/* tegra234.dtsi (vic@15340000, host1x@13e00000, smmu_niso1); BPMP     */
/* IDs from tegra234-{clock,reset,powergate}.h.  Register offsets and  */
/* programming sequences: mainline drivers/gpu/host1x (host1x08) +     */
/* drivers/gpu/drm/tegra/{vic,falcon}.c and Orin TRM DP-10508-002      */
/* §7.3 (VIC) / host1x chapter.                                        */
/* ================================================================== */
#define ORIN_VIC_PHY            0x15340000ULL
#define ORIN_VIC_SIZE           0x00040000u
#define ORIN_VIC_CLK            167u  /* CLK_VIC */
#define ORIN_VIC_RESET          113u  /* RESET_VIC */
#define ORIN_VIC_PG             29u   /* POWER_DOMAIN_VIC */
#define ORIN_VIC_SID            0x34u /* smmu_niso1 stream ID */

#define ORIN_HOST1X_COMMON_PHY  0x13e00000ULL
#define ORIN_HOST1X_HV_PHY      0x13e10000ULL
#define ORIN_HOST1X_VM_PHY      0x13e40000ULL
#define ORIN_HOST1X_APERTURE    0x00010000u
#define ORIN_HOST1X_CLK         46u   /* CLK_HOST1X */
#define ORIN_HOST1X_SID         0x27u /* smmu_niso1 fetch stream ID */

/* smmu_niso1: two interleaved MMU-500 instances, both must agree.
 * SMMUv2 global register space 0: stream matching is by SMR index, not
 * by stream ID, so S2CR(n) belongs to whichever SMR(n) matches the SID. */
#define ORIN_SMMU_NISO1_A_PHY   0x08000000ULL
#define ORIN_SMMU_NISO1_B_PHY   0x07000000ULL
#define ORIN_SMMU_GRO_SIZE      0x00010000u
#define ORIN_SMMU_SCR0          0x000u
#define ORIN_SMMU_SCR0_CLIENTPD (1u << 0)   /* 1 = SMMU off, all bypass */
#define ORIN_SMMU_SCR0_USFCFG   (1u << 10)  /* 1 = unmatched streams fault */
#define ORIN_SMMU_IDR0          0x020u      /* NUMSMRG in [7:0] */
#define ORIN_SMMU_SMR(n)        (0x800u + 4u * (n))
#define ORIN_SMMU_SMR_VALID     (1u << 31)
#define ORIN_SMMU_S2CR(n)       (0xc00u + 4u * (n))
#define ORIN_SMMU_S2CR_TYPE(v)  (((v) >> 16) & 3u) /* 0 trans 1 bypass 2 fault */

#endif