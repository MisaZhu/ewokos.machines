#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <bsp/bsp_pcie.h>
#include <bsp/bsp_bpmp.h>
#include <ewoksys/syscall.h>
#include <ewoksys/klog.h>
#include <orin_net.h>

/*
 * Tegra234 PCIe root-complex bring-up + enumeration (C1 WiFi / C8 Ethernet).
 *
 * Cold-own mode: BPMP power/clock/reset + P2U PHY + appl/DBI config + LTSSM
 * link training, for when UEFI power-gated the controller after
 * ExitBootServices.  After the link is up, the DesignWare iATU is programmed
 * (core reset cleared UEFI's regions) so config space and BAR MMIO translate:
 *
 *  - outbound region 0: CFG0/CFG1, dynamically retargeted per BDF, maps the
 *    256KB config window (c->cfg, e.g. C8 0x2a000000) to config TLPs.  This is
 *    the non-ECAM path (dw_pcie_other_conf_map_bus): the config reg is NOT
 *    256MB-aligned so Linux does not use native ECAM here.
 *  - outbound region 1: MEM, maps the non-prefetchable MMIO window
 *    (CPU 0x3528000000 -> PCI 0x40000000 on C8) so BAR accesses reach the EP.
 *
 * Register layout from upstream pcie-designware.c / pcie-tegra194.c.
 */

static inline void barrier(void) {
    __asm__ volatile("dsb sy" ::: "memory");
}

static int map_window(uint64_t va, uint64_t phy, uint32_t size) {
    ewokos_addr_t ret = syscall3(SYS_MEM_MAP, (ewokos_addr_t)va,
            (ewokos_addr_t)phy, (ewokos_addr_t)size);
    return ret == (ewokos_addr_t)va ? 0 : -1;
}

/* Translate a non-prefetchable PCI bus address to CPU physical through the
 * controller's MMIO range (tegra234.dtsi ranges).  C8: PCI 0x40000000 ->
 * CPU 0x3528000000; C1: PCI 0x40000000 -> CPU 0x20a8000000. */
static uint64_t bar_to_phys(int ctl, uint64_t bar_pci) {
    uint64_t pci = ORIN_PCIE_C8_MMIO_PCI;
    uint64_t phy = ORIN_PCIE_C8_MMIO_PHY;
    uint64_t size = ORIN_PCIE_C8_MMIO_SIZE;
    if (ctl == ORIN_PCIE_CTL_C1) {
        pci = ORIN_PCIE_C1_MMIO_PCI;
        phy = ORIN_PCIE_C1_MMIO_PHY;
        size = ORIN_PCIE_C1_MMIO_SIZE;
    }
    if (bar_pci >= pci && bar_pci < pci + size)
        return bar_pci - pci + phy;
    return 0;
}

/* ================================================================== */
/* Cold-own root-complex bring-up (BPMP + P2U + appl/DBI + LTSSM).      */
/* Sequence from upstream pcie-tegra194.c / phy-tegra194-p2u.c.         */
/* ================================================================== */

#define APPL_VA   0x64000000ULL
#define DBI_VA    0x64100000ULL
#define ATU_VA    0x64200000ULL
#define CFG_VA    0x64300000ULL
#define P2U0_VA   0x64400000ULL
#define P2U1_VA   0x64410000ULL

/* Tegra PCIe appl registers */
#define APPL_PINMUX        0x000
#define APPL_CTRL          0x004
#define APPL_DEBUG         0x0d0
#define APPL_DM_TYPE       0x100
#define APPL_CFG_BASE_ADDR 0x104
#define APPL_IATU_DMA_BASE 0x108
#define APPL_CFG_MISC      0x110
#define APPL_SLCG_OVERRIDE 0x114
#define APPL_CTRL_LTSSM_EN (1u << 7)
#define APPL_PINMUX_PEX_RST (1u << 0)
#define APPL_PINMUX_CLKREQ_OVERRIDE_EN (1u << 2)
#define APPL_PINMUX_CLKREQ_OVERRIDE (1u << 3)
#define APPL_PINMUX_CLKREQ_DEFAULT_VALUE (1u << 13)
#define LTSSM_L0           0x11u

/* DesignWare DBI registers */
#define DBI_PCI_COMMAND    0x004
#define DBI_BUS_NUM        0x018
#define DBI_BAR0_MASK      0x010
#define DBI_AMBA_ERR_RESP  0x8d0
#define DBI_GEN3_EQ_CTRL   0x148
#define DBI_IATU_VIEWPORT  0x900

/* P2U registers */
#define P2U_RX_DEBOUNCE    0x0a4
#define P2U_CONTROL_CMN    0x074
#define P2U_EQ_GEN3        0x0c0
#define P2U_EQ_GEN4        0x0c4
#define P2U_DIR_SEARCH     0x0d4

/* DesignWare unrolled iATU registers (pcie-designware.h).  Outbound region i
 * lives at atu_base + (i << 9); inbound at atu_base + (i << 9) + 0x100.
 * Tegra234 DWC core >= 5.10A: write UPPER_LIMIT, set DMA_BYPASS, no ECRC TD. */
#define ATU_REGION_CTRL1   0x000
#define ATU_REGION_CTRL2   0x004
#define ATU_LOWER_BASE     0x008
#define ATU_UPPER_BASE     0x00c
#define ATU_LIMIT          0x010
#define ATU_LOWER_TARGET   0x014
#define ATU_UPPER_TARGET   0x018
#define ATU_UPPER_LIMIT    0x020
#define ATU_ENABLE         (1u << 31)
#define ATU_DMA_BYPASS     (1u << 27)
#define ATU_TYPE_MEM       0x0u
#define ATU_TYPE_CFG0      0x4u
#define ATU_TYPE_CFG1      0x5u
#define ATU_BUS(x)         ((uint32_t)(x) << 24)
#define ATU_DEV(x)         ((uint32_t)(x) << 19)
#define ATU_FUNC(x)        ((uint32_t)(x) << 16)
#define CFG_WIN_SIZE       0x40000u

typedef struct {
    uint64_t appl, cfg, atu, dbi, ecam;
    uint64_t p2u[2];
    uint32_t lanes;
    uint32_t clk_core, clk_aux;
    uint32_t reset_core, reset_apb;
    uint32_t pg;
    uint32_t ctl_num;   /* PCIe controller number for BPMP ctrl_state (C1=1,C8=8) */
} pcie_ctl_t;

static const pcie_ctl_t _ctl[2] = {
    [ORIN_PCIE_CTL_C1] = {
        .appl = ORIN_PCIE_C1_APPL_PHY, .cfg = ORIN_PCIE_C1_CFG_PHY,
        .atu = ORIN_PCIE_C1_ATU_PHY, .dbi = ORIN_PCIE_C1_DBI_PHY,
        .ecam = ORIN_PCIE_C1_ECAM_PHY,
        .p2u = { ORIN_C1_P2U_PHY, 0 }, .lanes = 1,
        .clk_core = ORIN_C1_CLK_CORE, .clk_aux = ORIN_C1_CLK_MONITOR,
        .reset_core = ORIN_C1_RESET_CORE, .reset_apb = ORIN_C1_RESET_APB,
        .pg = ORIN_C1_PG, .ctl_num = 1,
    },
    [ORIN_PCIE_CTL_C8] = {
        .appl = ORIN_PCIE_C8_APPL_PHY, .cfg = ORIN_PCIE_C8_CFG_PHY,
        .atu = ORIN_PCIE_C8_ATU_PHY, .dbi = ORIN_PCIE_C8_DBI_PHY,
        .ecam = ORIN_PCIE_C8_ECAM_PHY,
        .p2u = { ORIN_C8_P2U0_PHY, ORIN_C8_P2U1_PHY }, .lanes = 2,
        .clk_core = ORIN_C8_CLK_CORE, .clk_aux = ORIN_C8_CLK_M,
        .reset_core = ORIN_C8_RESET_CORE, .reset_apb = ORIN_C8_RESET_APB,
        .pg = ORIN_C8_PG, .ctl_num = 8,
    },
};

static volatile uint8_t* _appl;
static volatile uint8_t* _dbi;
static volatile uint8_t* _atu;
static volatile uint8_t* _cfg;
static uint64_t _cfg_phys;
/* Cached BDF currently targeted by outbound iATU region 0 (config). */
static int _cfg_bus = -1, _cfg_dev = -1, _cfg_func = -1;

static inline uint32_t appl_rd(uint32_t off) {
    return *(volatile uint32_t*)(_appl + off);
}
static inline void appl_wr(uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(_appl + off) = v;
}
static inline void appl_set(uint32_t off, uint32_t set) {
    appl_wr(off, appl_rd(off) | set);
}
static inline void appl_clr(uint32_t off, uint32_t clr) {
    appl_wr(off, appl_rd(off) & ~clr);
}
static inline uint32_t dbi_rd(uint32_t off) {
    return *(volatile uint32_t*)(_dbi + off);
}
static inline void dbi_wr(uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(_dbi + off) = v;
}

static inline void atu_ob_wr(uint32_t idx, uint32_t reg, uint32_t v) {
    *(volatile uint32_t*)(_atu + (idx << 9) + reg) = v;
}
static inline uint32_t atu_ob_rd(uint32_t idx, uint32_t reg) {
    return *(volatile uint32_t*)(_atu + (idx << 9) + reg);
}

/* Program an outbound iATU region (unrolled), from dw_pcie_prog_outbound_atu:
 * CPU [cpu_addr, cpu_addr+size) -> PCI TLPs of `type` targeting pci_addr.
 * For CFG, pci_addr is the BDF (ATU_BUS|DEV|FUNC); for MEM it is the PCI bus
 * address.  limit_addr is on the CPU side. */
static int atu_prog_ob(uint32_t idx, uint32_t type, uint64_t cpu_addr,
        uint64_t pci_addr, uint32_t size) {
    uint64_t limit = cpu_addr + size - 1;
    atu_ob_wr(idx, ATU_LOWER_BASE, (uint32_t)cpu_addr);
    atu_ob_wr(idx, ATU_UPPER_BASE, (uint32_t)(cpu_addr >> 32));
    atu_ob_wr(idx, ATU_LIMIT, (uint32_t)limit);
    atu_ob_wr(idx, ATU_UPPER_LIMIT, (uint32_t)(limit >> 32));
    atu_ob_wr(idx, ATU_LOWER_TARGET, (uint32_t)pci_addr);
    atu_ob_wr(idx, ATU_UPPER_TARGET, (uint32_t)(pci_addr >> 32));
    atu_ob_wr(idx, ATU_REGION_CTRL1, type);
    barrier();
    atu_ob_wr(idx, ATU_REGION_CTRL2, ATU_ENABLE | ATU_DMA_BYPASS);
    barrier();
    for (uint32_t i = 0; i < 1000u; i++) {
        if (atu_ob_rd(idx, ATU_REGION_CTRL2) & ATU_ENABLE)
            return 0;
    }
    klog("orin pcie: iATU ob%u enable timeout ctrl2=%08x\n",
            idx, atu_ob_rd(idx, ATU_REGION_CTRL2));
    return -1;
}

/* MEM outbound region 1: non-prefetchable MMIO window so BAR accesses reach
 * the endpoint (dw_pcie_iatu_setup MEM range, index 1 in non-ECAM mode). */
static int pcie_setup_mem_atu(int ctl) {
    uint64_t cpu = ORIN_PCIE_C8_MMIO_PHY, pci = ORIN_PCIE_C8_MMIO_PCI;
    uint32_t size = ORIN_PCIE_C8_MMIO_SIZE;
    if (ctl == ORIN_PCIE_CTL_C1) {
        cpu = ORIN_PCIE_C1_MMIO_PHY;
        pci = ORIN_PCIE_C1_MMIO_PCI;
        size = ORIN_PCIE_C1_MMIO_SIZE;
    }
    return atu_prog_ob(1, ATU_TYPE_MEM, cpu, pci, size);
}

/* Retarget config region 0 to a BDF (dw_pcie_other_conf_map_bus).  bus 1 is
 * CFG0 (immediate child of root bus 0); deeper buses are CFG1. */
static int cfg_select(uint8_t bus, uint8_t dev, uint8_t func) {
    if (bus == _cfg_bus && dev == _cfg_dev && func == _cfg_func)
        return 0;
    uint32_t type = (bus == 1) ? ATU_TYPE_CFG0 : ATU_TYPE_CFG1;
    uint64_t bdf = ATU_BUS(bus) | ATU_DEV(dev) | ATU_FUNC(func);
    if (atu_prog_ob(0, type, _cfg_phys, bdf, CFG_WIN_SIZE) != 0)
        return -1;
    _cfg_bus = bus; _cfg_dev = dev; _cfg_func = func;
    return 0;
}

static uint32_t cfg_read32(uint8_t bus, uint8_t dev, uint8_t func, uint32_t reg) {
    if (bus == 0)
        return (dev == 0) ? dbi_rd(reg & 0xfff) : 0xffffffffu;
    if (cfg_select(bus, dev, func) != 0)
        return 0xffffffffu;
    return *(volatile uint32_t*)(_cfg + (reg & 0xfff));
}

static void cfg_write32(uint8_t bus, uint8_t dev, uint8_t func, uint32_t reg,
        uint32_t val) {
    if (bus == 0) {
        if (dev == 0)
            dbi_wr(reg & 0xfff, val);
        return;
    }
    if (cfg_select(bus, dev, func) != 0)
        return;
    *(volatile uint32_t*)(_cfg + (reg & 0xfff)) = val;
}

/* Probe a BAR: write all-ones, read back the size mask, restore the original.
 * Returns the mask readback (0 = BAR not implemented). */
static uint32_t bar_probe(uint8_t bus, uint8_t dev, uint8_t func, uint32_t reg) {
    uint32_t orig = cfg_read32(bus, dev, func, reg);
    cfg_write32(bus, dev, func, reg, 0xffffffffu);
    uint32_t mask = cfg_read32(bus, dev, func, reg);
    cfg_write32(bus, dev, func, reg, orig);
    return mask;
}

/* Find the first memory BAR (r8169 "use first MMIO region"), probing its size.
 * Skips IO BARs; a 64-bit MEM BAR consumes the next slot.  Returns 0 and fills
 * the BAR register offset, size (>= 4KB) and 64-bit flag. */
static int find_mem_bar(uint8_t bus, uint8_t dev, uint8_t func,
        uint32_t* reg_out, uint64_t* size_out, int* is64_out) {
    for (uint32_t slot = 0; slot < 6; slot++) {
        uint32_t reg = 0x10 + slot * 4;
        uint32_t mask = bar_probe(bus, dev, func, reg);
        if (mask == 0 || (mask & 1u))
            continue;                 /* unimplemented or IO */
        int is64 = ((mask & 0x06) == 0x04);
        uint64_t m = (uint64_t)(mask & ~0xfu);
        if (is64) {
            m |= (uint64_t)bar_probe(bus, dev, func, reg + 4) << 32;
            slot++;                   /* consumed the upper half */
        }
        uint64_t size = (~m) + 1;
        if (size < 0x1000)
            size = 0x1000;
        *reg_out = reg;
        *size_out = size;
        *is64_out = is64;
        return 0;
    }
    return -1;
}

static void p2u_init(uint64_t va) {
    volatile uint8_t* p = (volatile uint8_t*)(uintptr_t)va;
    *(volatile uint32_t*)(p + P2U_RX_DEBOUNCE) = 160;
    uint32_t cmn = *(volatile uint32_t*)(p + P2U_CONTROL_CMN);
    *(volatile uint32_t*)(p + P2U_CONTROL_CMN) = cmn | (1u << 20);
    uint32_t g3 = *(volatile uint32_t*)(p + P2U_EQ_GEN3);
    *(volatile uint32_t*)(p + P2U_EQ_GEN3) = (g3 & ~1u) | (1u << 1);
    uint32_t g4 = *(volatile uint32_t*)(p + P2U_EQ_GEN4);
    *(volatile uint32_t*)(p + P2U_EQ_GEN4) = g4 | (1u << 1);
    uint32_t ds = *(volatile uint32_t*)(p + P2U_DIR_SEARCH);
    *(volatile uint32_t*)(p + P2U_DIR_SEARCH) = ds & ~(1u << 18);
    barrier();
}

static int rc_map_windows(const pcie_ctl_t* c) {
    if (map_window(APPL_VA, c->appl, 0x20000) != 0 ||
            map_window(DBI_VA, c->dbi, 0x40000) != 0 ||
            map_window(ATU_VA, c->atu, 0x40000) != 0 ||
            map_window(CFG_VA, c->cfg, 0x40000) != 0 ||
            map_window(P2U0_VA, c->p2u[0], 0x10000) != 0)
        return -1;
    if (c->lanes > 1 && map_window(P2U1_VA, c->p2u[1], 0x10000) != 0)
        return -1;
    _appl = (volatile uint8_t*)(uintptr_t)APPL_VA;
    _dbi = (volatile uint8_t*)(uintptr_t)DBI_VA;
    _atu = (volatile uint8_t*)(uintptr_t)ATU_VA;
    _cfg = (volatile uint8_t*)(uintptr_t)CFG_VA;
    _cfg_phys = c->cfg;
    return 0;
}

int bsp_pcie_cold_own(int ctl) {
    if (ctl < 0 || ctl > 1)
        return -1;
    const pcie_ctl_t* c = &_ctl[ctl];

    if (bsp_bpmp_init() != 0)
        return -1;

    /* 1. Powergate on, then BPMP controller-state enable.  ctrl_state is THE
     * gate that un-gates the controller (incl. the DBI/core clock domain)
     * inside BPMP firmware; without it clk_enable returns 0 but the first DBI
     * access RAS-faults.  Fatal on failure to avoid that hang. */
    int r;
    r = bsp_bpmp_pg_on(c->pg);
    if (r != 0) {
        klog("orin pcie: ctl%d powergate(%u) on failed err=%d\n", ctl, c->pg, r);
        return -1;
    }
    r = bsp_bpmp_uphy_ctrl_state(c->ctl_num, 1);
    klog("orin pcie: ctl%d ctrl_state(%u)=%d\n", ctl, c->ctl_num, r);
    if (r != 0) {
        klog("orin pcie: ctl%d ctrl_state err=%d (fatal; DBI would RAS)\n", ctl, r);
        return -1;
    }
    r = bsp_bpmp_clk_enable(c->clk_core);
    klog("orin pcie: ctl%d clk_core(%u)=%d\n", ctl, c->clk_core, r);
    if (c->clk_aux) {
        r = bsp_bpmp_clk_enable(c->clk_aux);
        klog("orin pcie: ctl%d clk_aux(%u)=%d\n", ctl, c->clk_aux, r);
    }
    r = bsp_bpmp_reset_deassert(c->reset_apb);
    klog("orin pcie: ctl%d reset_apb(%u)=%d\n", ctl, c->reset_apb, r);
    klog("orin pcie: ctl%d BPMP power/clock/reset done\n", ctl);

    if (rc_map_windows(c) != 0) {
        klog("orin pcie: ctl%d register map failed\n", ctl);
        return -1;
    }
    klog("orin pcie: ctl%d windows mapped\n", ctl);

    /* 2. P2U PHY init per lane (upstream tegra_pcie_enable_phy: phy_init +
     * phy_power_on before appl config).  The pll_init MRQ is EP-controller-
     * only (invalid for a root port), so skip PLL and rely on firmware NVHS
     * setup, but re-tune the P2U lanes after the power-gate cycle. */
    p2u_init(P2U0_VA);
    if (c->lanes > 1)
        p2u_init(P2U1_VA);
    klog("orin pcie: ctl%d p2u phy init done (%u lanes)\n", ctl, c->lanes);

    /* 3. appl config (apb domain): RP mode, DBI/iATU base, cache attrs.
     * Done BEFORE deasserting core reset, per upstream config_controller. */
    appl_wr(APPL_DM_TYPE, 4);                 /* root port */
    appl_wr(APPL_CFG_BASE_ADDR, (uint32_t)c->dbi);
    appl_wr(APPL_IATU_DMA_BASE, (uint32_t)c->atu);
    appl_wr(APPL_SLCG_OVERRIDE, 0);
    appl_wr(APPL_CFG_MISC, (appl_rd(APPL_CFG_MISC) & ~(3u << 10)) | (3u << 10));
    appl_set(APPL_CTRL, 1u << 6);
    /* C8 has no supports-clkreq: force CLKREQ# active so the RP supplies
     * REFCLK to the endpoint (upstream config_controller !supports_clkreq).
     * Without this the EP gets no refclk and LTSSM stalls in Polling. */
    appl_wr(APPL_PINMUX, (appl_rd(APPL_PINMUX) |
                APPL_PINMUX_CLKREQ_OVERRIDE_EN) &
                ~(APPL_PINMUX_CLKREQ_OVERRIDE | APPL_PINMUX_CLKREQ_DEFAULT_VALUE));
    barrier();
    klog("orin pcie: ctl%d appl configured (pinmux=%x)\n", ctl, appl_rd(APPL_PINMUX));

    /* 4. Deassert core reset LAST: brings the DesignWare DBI domain out of
     * reset so DBI registers become accessible. */
    r = bsp_bpmp_reset_deassert(c->reset_core);
    klog("orin pcie: ctl%d reset_core(%u)=%d\n", ctl, c->reset_core, r);
    barrier();
    usleep(1000);
    klog("orin pcie: ctl%d dbi begin (appl_ctrl=%x)\n", ctl, appl_rd(APPL_CTRL));
    klog("orin pcie: ctl%d dbi[0]=%08x (RP vendor/device)\n", ctl, dbi_rd(0));
    dbi_wr(DBI_BAR0_MASK, 0);
    dbi_wr(DBI_BUS_NUM, 0x00010100);          /* pri 0, sec 1, sub 1 */
    dbi_wr(DBI_AMBA_ERR_RESP, 0xffff0001);    /* RRS for config reads */
    dbi_wr(DBI_GEN3_EQ_CTRL, 5);              /* Gen3 EQ preset 5 */
    dbi_wr(DBI_PCI_COMMAND, dbi_rd(DBI_PCI_COMMAND) | 0x06);
    barrier();
    klog("orin pcie: ctl%d appl/DBI configured (dm_type=%u)\n",
            ctl, appl_rd(APPL_DM_TYPE));

    /* 4b. Program the MEM outbound iATU (region 1) for BAR MMIO.  Core reset
     * cleared UEFI's iATU; without this, BAR reads/writes never reach the EP.
     * Config region 0 is programmed on demand during enumeration. */
    r = pcie_setup_mem_atu(ctl);
    klog("orin pcie: ctl%d mem iATU=%d viewport=%08x ob1.ctrl2=%08x\n",
            ctl, r, dbi_rd(DBI_IATU_VIEWPORT), atu_ob_rd(1, ATU_REGION_CTRL2));

    /* 5. Link training (upstream tegra_pcie_dw_start_link): assert PEX_RST,
     * enable LTSSM, then DEASSERT PEX_RST so the endpoint leaves reset and
     * trains.  PEX_RST bit0: 0=asserted, 1=deasserted. */
    appl_clr(APPL_PINMUX, APPL_PINMUX_PEX_RST);
    usleep(100);
    appl_set(APPL_CTRL, APPL_CTRL_LTSSM_EN);
    appl_set(APPL_PINMUX, APPL_PINMUX_PEX_RST);
    barrier();

    int link_up = 0;
    for (uint32_t i = 0; i < 1200u; i++) {     /* ~120ms */
        uint32_t ltssm = (appl_rd(APPL_DEBUG) >> 3) & 0x3f;
        if (ltssm == LTSSM_L0) {
            link_up = 1;
            break;
        }
        usleep(100);
    }
    if (!link_up) {
        klog("orin pcie: ctl%d link training timeout LTSSM=%u\n",
                ctl, (appl_rd(APPL_DEBUG) >> 3) & 0x3f);
        return -1;
    }
    klog("orin pcie: ctl%d link up (L0)\n", ctl);
    return 0;
}

int bsp_pcie_find_device(int ctl, uint16_t vendor, uint16_t device,
        orin_pcie_dev_t* out) {
    if (ctl < 0 || ctl > 1 || out == NULL)
        return -1;
    const pcie_ctl_t* c = &_ctl[ctl];
    memset(out, 0, sizeof(*out));

    /* Config window / iATU / DBI are mapped by cold_own; map defensively if
     * called standalone. */
    if (_cfg == NULL || _atu == NULL || _dbi == NULL) {
        if (rc_map_windows(c) != 0) {
            klog("orin pcie: ctl%d register map failed\n", ctl);
            return -1;
        }
    }
    _cfg_phys = c->cfg;
    _cfg_bus = _cfg_dev = _cfg_func = -1;

    int found = -1;
    /* Root port is bus 0 (DBI); the endpoint is normally bus 1 dev 0.  Scan a
     * small range to tolerate firmware bus-numbering differences. */
    for (uint8_t bus = 0; bus <= 2 && found != 0; bus++) {
        for (uint8_t dev = 0; dev <= 1 && found != 0; dev++) {
            uint32_t id = cfg_read32(bus, dev, 0, 0x00);
            if ((uint16_t)(id & 0xffff) != vendor)
                continue;
            if ((uint16_t)(id >> 16) != device)
                continue;
            /* Cold-own hot-reset the EP, so UEFI's BAR assignment is gone.
             * Probe the first MEM BAR, assign a PCI address in the controller
             * MMIO window, then enable memory-space + bus-master. */
            uint32_t bar_reg; uint64_t bar_size; int bar64;
            if (find_mem_bar(bus, dev, 0, &bar_reg, &bar_size, &bar64) != 0) {
                klog("orin pcie: ctl%d %02x:%02x.0 no MEM BAR\n", ctl, bus, dev);
                continue;
            }
            uint64_t mmio_pci = (ctl == ORIN_PCIE_CTL_C1) ?
                    ORIN_PCIE_C1_MMIO_PCI : ORIN_PCIE_C8_MMIO_PCI;
            uint64_t bar_pci = (mmio_pci + bar_size - 1) & ~(bar_size - 1);
            cfg_write32(bus, dev, 0, bar_reg, (uint32_t)bar_pci);
            if (bar64)
                cfg_write32(bus, dev, 0, bar_reg + 4, (uint32_t)(bar_pci >> 32));
            uint32_t cmd = cfg_read32(bus, dev, 0, 0x04);
            cfg_write32(bus, dev, 0, 0x04, cmd | 0x06);
            uint64_t phys = bar_to_phys(ctl, bar_pci);
            if (phys == 0) {
                klog("orin pcie: ctl%d BAR pci=%llx outside MMIO window\n",
                        ctl, (unsigned long long)bar_pci);
                continue;
            }
            out->vendor_id = vendor;
            out->device_id = device;
            out->bus = bus;
            out->dev = dev;
            out->func = 0;
            out->bar0_phys = phys;
            out->bar0_size = (uint32_t)bar_size;
            klog("orin pcie: ctl%d %04x:%04x @%02x:%02x.0 BAR%u(%s) size=%llx pci=%llx phys=%llx cmd=%04x\n",
                    ctl, vendor, device, bus, dev, (bar_reg - 0x10) / 4,
                    bar64 ? "64" : "32", (unsigned long long)bar_size,
                    (unsigned long long)bar_pci, (unsigned long long)phys,
                    cfg_read32(bus, dev, 0, 0x04) & 0xffff);
            found = 0;
        }
    }
    if (found != 0)
        klog("orin pcie: ctl%d %04x:%04x not found (bus1dev0 id=%08x)\n",
                ctl, vendor, device, cfg_read32(1, 0, 0, 0x00));
    return found;
}