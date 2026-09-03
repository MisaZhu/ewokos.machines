#include <arch/bcm2712/mmio.h>
#include <arch/bcm2712/rp1.h>
#include <ewoksys/klog.h>
#include <ewoksys/mmio.h>
#include <ewoksys/sys.h>
#include <ewoksys/syscall.h>
#include <unistd.h>

#define PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1         0x0188
#define PCIE_RC_CFG_PRIV1_ID_VAL3                       0x043c
#define PCIE_RC_CFG_PRIV1_LINK_CAPABILITY               0x04dc
#define PCIE_RC_DL_MDIO_ADDR                            0x1100
#define PCIE_RC_DL_MDIO_WR_DATA                         0x1104
#define PCIE_RC_PL_PHY_CTL_15                           0x184c
#define PCIE_EXT_CFG_DATA                               0x8000
#define PCIE_EXT_CFG_INDEX                              0x9000
#define PCIE_MISC_MISC_CTRL                             0x4008
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO                0x400c
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_HI                0x4010
#define PCIE_MISC_RC_BAR1_CONFIG_LO                     0x402c
#define PCIE_MISC_RC_BAR1_CONFIG_HI                     0x4030
#define PCIE_MISC_RC_BAR2_CONFIG_LO                     0x4034
#define PCIE_MISC_RC_BAR2_CONFIG_HI                     0x4038
#define PCIE_MISC_RC_BAR3_CONFIG_LO                     0x403c
#define PCIE_MISC_RC_CONFIG_RETRY_TIMEOUT               0x405c
#define PCIE_MISC_PCIE_CTRL                             0x4064
#define PCIE_MISC_PCIE_STATUS                           0x4068
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT        0x4070
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI           0x4080
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI          0x4084
#define PCIE_MISC_UBUS_CTRL                             0x40a4
#define PCIE_MISC_UBUS_TIMEOUT                          0x40a8
#define PCIE_MISC_UBUS_BAR1_CONFIG_REMAP                0x40ac
#define PCIE_MISC_UBUS_BAR1_CONFIG_REMAP_HI             0x40b0
#define PCIE_MISC_UBUS_BAR2_CONFIG_REMAP                0x40b4
#define PCIE_MISC_HARD_PCIE_HARD_DEBUG                  0x4304
#define PCIE_MISC_AXI_READ_ERROR_DATA                   0x4170

/* QoS forwarding of RP1's VDM-tagged traffic (Linux brcm_pcie_set_tc_qos) */
#define PCIE_RC_TL_VDM_CTL1                             0x0a0c
#define PCIE_RC_TL_VDM_CTL0                             0x0a20
#define  VDM_CTL0_VDM_ENABLED                           (1u << 16)
#define  VDM_CTL0_VDM_IGNORETAG                         (1u << 17)
#define  VDM_CTL0_VDM_IGNOREVNDRID                      (1u << 18)
#define PCIE_MISC_CTRL_1                                0x40a0
#define  MISC_CTRL_1_EN_VDM_QOS_CONTROL                 (1u << 5)
#define PCIE_MISC_VDM_PRIORITY_TO_QOS_MAP_HI            0x4164
#define PCIE_MISC_VDM_PRIORITY_TO_QOS_MAP_LO            0x4168
#define PCIE_MISC_AXI_INTF_CTRL                         0x416c
#define  AXI_EN_RCLK_QOS_ARRAY_FIX                      (1u << 13)
#define  AXI_EN_QOS_UPDATE_TIMING_FIX                   (1u << 12)
#define  AXI_DIS_QOS_GATING_IN_MASTER                   (1u << 11)
#define  AXI_REQFIFO_EN_QOS_PROPAGATION                 (1u << 7)
#define  AXI_MASTER_MAX_OUTSTANDING_REQUESTS_MASK       0x3f
/* Pi 5 device tree: rp1_target: &pcie2 { brcm,vdm-qos-map = <0xbbaa9888>; } */
#define VDM_QOS_MAP                                     0xbbaa9888u

#define PCI_COMMAND              0x04
#define PCI_CACHE_LINE_SIZE      0x0c
#define PCI_SECONDARY_BUS        0x19
#define PCI_SUBORDINATE_BUS      0x1a
#define PCI_MEMORY_BASE          0x20
#define PCI_MEMORY_LIMIT         0x22
#define PCI_BRIDGE_CONTROL       0x3e
#define PCI_BASE_ADDRESS_0       0x10
#define PCI_BASE_ADDRESS_1       0x14
#define PCI_BASE_ADDRESS_2       0x18
#define PCI_BASE_ADDRESS_3       0x1c
#define PCI_EXP_LNKCAP           0x0c
#define  PCI_EXP_LNKCAP_ASPM_MASK   (3u << 10)
#define  PCI_EXP_LNKCAP_CLKPM       (1u << 18)
#define PCI_EXP_LNKCTL           0x10
#define  PCI_EXP_LNKCTL_ASPMC_MASK  0x0003
#define  PCI_EXP_LNKCTL_CLKPM_EN    (1u << 8)
#define PCI_EXP_LNKSTA           0x12
#define PCI_EXP_LNKCTL2          0x30
#define  PCI_EXP_LNKCTL2_HW_SPEED_DIS (1u << 5)
#define PCI_CAP_PTR              0x34
#define PCI_CAP_ID_EXP           0x10
/* extended capability chain (4KB config space) */
#define PCI_EXT_CAP_START        0x100
#define PCI_EXT_CAP_ID_L1SS      0x1e
#define PCI_L1SS_CTL1            0x08
#define  PCI_L1SS_CTL1_MASK      0x000fu /* PCIPM/ASPM L1.2 + L1.1 enables */
#define BRCM_PCIE_CAP_REGS       0x00ac

#define PCI_COMMAND_MEMORY       0x0002
#define PCI_COMMAND_MASTER       0x0004
#define PCI_COMMAND_PARITY       0x0040
#define PCI_COMMAND_SERR         0x0100
#define PCI_BRIDGE_CTL_PARITY    0x0001
#define PCI_BASE_ADDRESS_MEM_64  0x00000004

#define PCIE_STATUS_PHY_LINK_UP  (1u << 4)
#define PCIE_STATUS_DL_ACTIVE    (1u << 5)
#define MDIO_DONE                0x80000000u

static int rp1_ready;

static inline void write8(ewokos_addr_t addr, uint8_t value) {
    *((volatile uint8_t *)addr) = value;
}

static inline uint8_t read8(ewokos_addr_t addr) {
    return *((volatile uint8_t *)addr);
}

static inline uint16_t read16(ewokos_addr_t addr) {
    return *((volatile uint16_t *)addr);
}

static inline void write16(ewokos_addr_t addr, uint16_t value) {
    *((volatile uint16_t *)addr) = value;
}

static void update32(ewokos_addr_t addr, uint32_t clear, uint32_t set) {
    put32(addr, (get32(addr) & ~clear) | set);
}

static inline ewokos_addr_t align_down_addr(ewokos_addr_t value, uint32_t align) {
    return value & ~((ewokos_addr_t)align - 1u);
}

static int get_runtime_page_size(uint32_t* page_size) {
    sys_info_t sysinfo;

    if (page_size == NULL)
        return -1;
    if (sys_get_sys_info(&sysinfo) != 0)
        return -1;
    if (sysinfo.page_size < 4096 ||
            (sysinfo.page_size & (sysinfo.page_size - 1u)) != 0)
        return -1;

    *page_size = sysinfo.page_size;
    return 0;
}

static int map_page_window(ewokos_addr_t virt, ewokos_addr_t phys, uint32_t size) {
    return syscall3(SYS_MEM_MAP, virt, phys, size) == virt ? 0 : -1;
}

static int map_subpage_window(ewokos_addr_t virt_addr, uint64_t phys,
        uint32_t page_size, ewokos_addr_t* mapped) {
    ewokos_addr_t phys_base = align_down_addr((ewokos_addr_t)phys, page_size);
    ewokos_addr_t virt_base = align_down_addr(virt_addr, page_size);
    ewokos_addr_t phys_off = (ewokos_addr_t)phys - phys_base;
    ewokos_addr_t virt_off = virt_addr - virt_base;

    if(virt_off != phys_off)
        return -1;

    if(map_page_window(virt_base, phys_base, page_size) != 0)
        return -1;

    *mapped = virt_addr;
    return 0;
}

static int ilog2_u64(uint64_t value) {
    int result = 0;
    while ((1ULL << result) < value)
        result++;
    return result;
}

static uint32_t encode_ibar_size(uint64_t size) {
    int log2 = ilog2_u64(size);
    if (log2 >= 12 && log2 <= 15)
        return (uint32_t)(log2 - 12 + 0x1c);
    if (log2 >= 16 && log2 <= 37)
        return (uint32_t)(log2 - 15);
    return 0;
}

static int mdio_write(ewokos_addr_t host, uint8_t reg, uint16_t data) {
    put32(host + PCIE_RC_DL_MDIO_ADDR, reg);
    (void)get32(host + PCIE_RC_DL_MDIO_ADDR);
    put32(host + PCIE_RC_DL_MDIO_WR_DATA, MDIO_DONE | data);
    for (int i = 0; i < 10; i++) {
        if (!(get32(host + PCIE_RC_DL_MDIO_WR_DATA) & MDIO_DONE))
            return 0;
        usleep(10);
    }
    return -1;
}

static void munge_pll(ewokos_addr_t host) {
    static const uint8_t regs[] = {0x16, 0x17, 0x18, 0x19, 0x1b, 0x1c, 0x1e};
    static const uint16_t data[] = {0x50b9, 0xbda1, 0x0094, 0x97b4, 0x5030, 0x5030, 0x0007};
    mdio_write(host, 0x1f, 0x1600);
    for (unsigned i = 0; i < sizeof(regs); i++)
        mdio_write(host, regs[i], data[i]);
    usleep(100);
}

static int link_up(ewokos_addr_t host) {
    uint32_t status = get32(host + PCIE_MISC_PCIE_STATUS);
    return (status & (PCIE_STATUS_PHY_LINK_UP | PCIE_STATUS_DL_ACTIVE)) ==
        (PCIE_STATUS_PHY_LINK_UP | PCIE_STATUS_DL_ACTIVE);
}

/*
 * Forward RP1's VDM-tagged traffic priorities to the RC's AXI QoS
 * interface — Linux brcm_pcie_set_tc_qos() driven by the Pi 5 device
 * tree's "brcm,vdm-qos-map = <0xbbaa9888>" on pcie2. RP1 marks its
 * real-time flows (DPI/DSI scan-out fetch, CSI) with VDM priorities;
 * without this map those reads contend with CPU traffic at the DRAM
 * controller on equal terms, so the display DMA underflows whenever
 * the CPU streams memory (irregular banding/shift only while the
 * screen is being redrawn, static frames fine).
 */
static void tc_qos_program(ewokos_addr_t host) {
    /* disable broken QoS forwarding search, set 2712D0 chicken bits */
    update32(host + PCIE_MISC_AXI_INTF_CTRL,
        AXI_REQFIFO_EN_QOS_PROPAGATION,
        AXI_EN_RCLK_QOS_ARRAY_FIX | AXI_EN_QOS_UPDATE_TIMING_FIX |
        AXI_DIS_QOS_GATING_IN_MASTER);
    /* timing-fix bit reserved-0 means 2712C1: best-effort alternative
       is to throttle in-flight AXI requests to the SDC */
    if (!(get32(host + PCIE_MISC_AXI_INTF_CTRL) & AXI_EN_QOS_UPDATE_TIMING_FIX))
        update32(host + PCIE_MISC_AXI_INTF_CTRL,
            AXI_MASTER_MAX_OUTSTANDING_REQUESTS_MASK, 15);

    /* map every VDM priority index to elevated QoS levels 8..11 */
    update32(host + PCIE_MISC_CTRL_1, 0, MISC_CTRL_1_EN_VDM_QOS_CONTROL);
    put32(host + PCIE_MISC_VDM_PRIORITY_TO_QOS_MAP_LO, VDM_QOS_MAP);
    put32(host + PCIE_MISC_VDM_PRIORITY_TO_QOS_MAP_HI, VDM_QOS_MAP);

    /* match vendor ID 0, forward VDMs to the priority interface */
    put32(host + PCIE_RC_TL_VDM_CTL1, 0);
    update32(host + PCIE_RC_TL_VDM_CTL0, 0,
        VDM_CTL0_VDM_ENABLED | VDM_CTL0_VDM_IGNORETAG |
        VDM_CTL0_VDM_IGNOREVNDRID);
}

static int tc_qos_ok(ewokos_addr_t host) {
    return get32(host + PCIE_MISC_VDM_PRIORITY_TO_QOS_MAP_LO) == VDM_QOS_MAP &&
        get32(host + PCIE_MISC_VDM_PRIORITY_TO_QOS_MAP_HI) == VDM_QOS_MAP &&
        (get32(host + PCIE_MISC_CTRL_1) & MISC_CTRL_1_EN_VDM_QOS_CONTROL) != 0 &&
        (get32(host + PCIE_RC_TL_VDM_CTL0) & VDM_CTL0_VDM_ENABLED) != 0;
}

static void set_tc_qos(ewokos_addr_t host) {
    tc_qos_program(host);
    /* the DSI scan-out silently falls back to default DRAM priority if
       any of this did not stick (the load-dependent banding returns),
       so verify by readback and retry once before giving up */
    if (!tc_qos_ok(host)) {
        tc_qos_program(host);
        if (!tc_qos_ok(host))
            klog("rp1-pcie: QoS map did not stick, display fetch runs at "
                "default priority\n");
    }
    klog("rp1-pcie: qos map=%08x/%08x ctrl1=%08x axi=%08x vdm=%08x\n",
        get32(host + PCIE_MISC_VDM_PRIORITY_TO_QOS_MAP_LO),
        get32(host + PCIE_MISC_VDM_PRIORITY_TO_QOS_MAP_HI),
        get32(host + PCIE_MISC_CTRL_1),
        get32(host + PCIE_MISC_AXI_INTF_CTRL),
        get32(host + PCIE_RC_TL_VDM_CTL0));
}

/* PCI Express extended capability chain walk (RC config space) */
static uint32_t find_ext_cap(ewokos_addr_t cfg, uint16_t id) {
    uint32_t off = PCI_EXT_CAP_START;

    for (int hops = 0; hops < 64 &&
            off >= PCI_EXT_CAP_START && off < 0x1000; hops++) {
        uint32_t hdr = get32(cfg + off);
        if (hdr == 0 || hdr == 0xffffffffu)
            break;
        if ((hdr & 0xffffu) == id)
            return off;
        off = (hdr >> 20) & 0xffcu;
    }
    return 0;
}

/*
 * Pin the link's dynamic power/speed management. RP1's DSI scan-out
 * rides this link at a fixed byte rate; any ASPM L0s/L1 entry, reference
 * clock gating or autonomous speed change varies the fetch
 * latency/bandwidth over time — the panel then sees the display's
 * refresh rate and bandwidth being adjusted underneath it (DMA
 * underflow, MIPI bitclock drift). The DPI engine has no rate control
 * of its own, so freezing the link freezes the display. The same wakeup
 * latency hits every RP1 register access, which is why the I2C touch
 * path slows down at the same time.
 */
static void lock_link_state(ewokos_addr_t host) {
    uint32_t lnkcap0 = get32(host + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCAP);
    uint16_t lnkctl0 = read16(host + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCTL);
    uint16_t lnksta = read16(host + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKSTA);
    uint32_t l1ss;

    /* what the firmware armed before the pinning: non-zero ASPM/CLKPM
     * here is exactly the dynamic power management that drops the link
     * speed and gates its reference clock whenever the link looks idle,
     * and makes every later I2C MMIO access and display fetch pay the
     * wakeup latency */
    klog("rp1-pcie: link gen%u x%u lnkcap=%08x lnkctl=%04x -> pinning PM off\n",
        lnksta & 0xfu, (lnksta >> 4) & 0x3fu, lnkcap0, lnkctl0);

    put32(host + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCAP,
        lnkcap0 & ~(PCI_EXP_LNKCAP_ASPM_MASK | PCI_EXP_LNKCAP_CLKPM));
    write16(host + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCTL,
        lnkctl0 & ~(PCI_EXP_LNKCTL_ASPMC_MASK | PCI_EXP_LNKCTL_CLKPM_EN));
    uint16_t lnkctl2 = read16(host + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCTL2);
    write16(host + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCTL2,
        lnkctl2 | PCI_EXP_LNKCTL2_HW_SPEED_DIS);

    /* no deep L1 substates either; the substates activate only when both
     * ports enable them, so disabling them on the RC side is enough */
    l1ss = find_ext_cap(host, PCI_EXT_CAP_ID_L1SS);
    if (l1ss != 0)
        update32(host + l1ss + PCI_L1SS_CTL1, PCI_L1SS_CTL1_MASK, 0);
}

/*
 * RP1 endpoint side of the same pinning. The PCIe capability's offset
 * varies between RP1 revisions, so walk the capability list.
 */
static void lock_endpoint_state(ewokos_addr_t rp1_cfg) {
    uint8_t ptr = read8(rp1_cfg + PCI_CAP_PTR) & ~3u;

    for (int hops = 0; ptr >= 0x40 && hops < 16; hops++) {
        uint8_t id = read8(rp1_cfg + ptr);
        if (id == 0)
            break;
        if (id == PCI_CAP_ID_EXP) {
            uint16_t lnkctl = read16(rp1_cfg + ptr + PCI_EXP_LNKCTL);
            write16(rp1_cfg + ptr + PCI_EXP_LNKCTL,
                lnkctl & ~(PCI_EXP_LNKCTL_ASPMC_MASK | PCI_EXP_LNKCTL_CLKPM_EN));
            uint16_t lnkctl2 = read16(rp1_cfg + ptr + PCI_EXP_LNKCTL2);
            write16(rp1_cfg + ptr + PCI_EXP_LNKCTL2,
                lnkctl2 | PCI_EXP_LNKCTL2_HW_SPEED_DIS);
            return;
        }
        ptr = read8(rp1_cfg + ptr + 1) & ~3u;
    }
}

static int train_link(ewokos_addr_t host, ewokos_addr_t reset, ewokos_addr_t rescal) {
    /* RESCAL deassert, from Circle's rescal_reset_deassert(). */
    uint32_t value = get32(rescal + 0x500);
    put32(rescal + 0x500, value | 1);
    if (!(get32(rescal + 0x500) & 1))
        return -1;
    for (int i = 0; i < 40 && !(get32(rescal + 0x508) & 1); i++)
        usleep(25);
    if (!(get32(rescal + 0x508) & 1))
        return -2;
    put32(rescal + 0x500, get32(rescal + 0x500) & ~1u);

    /* Assert/deassert reset 44 (bank 1, bit 12). */
    put32(reset + 0x318 + 0x18, 1u << 12);
    usleep(100);
    put32(reset + 0x318 + 0x18 + 4, 1u << 12);
    usleep(100);
    update32(host + PCIE_MISC_HARD_PCIE_HARD_DEBUG, 1u << 27, 0);
    usleep(100);

    munge_pll(host);
    update32(host + PCIE_RC_PL_PHY_CTL_15, 0xff, 0x12);
    update32(host + PCIE_MISC_MISC_CTRL, 0x00303400,
        (1u << 12) | (1u << 13) | (1u << 20) | (1u << 10));

    /* 64GB inbound DMA window at PCIe 0x10_00000000. */
    put32(host + PCIE_MISC_RC_BAR2_CONFIG_LO, encode_ibar_size(0x1000000000ULL));
    put32(host + PCIE_MISC_RC_BAR2_CONFIG_HI, 0x10);
    update32(host + PCIE_MISC_UBUS_BAR2_CONFIG_REMAP, 0, 1);
    update32(host + PCIE_MISC_MISC_CTRL, 0xf8000000, 21u << 27);
    update32(host + PCIE_MISC_UBUS_CTRL, 0, (1u << 13) | (1u << 19));
    put32(host + PCIE_MISC_AXI_READ_ERROR_DATA, 0xffffffff);
    put32(host + PCIE_MISC_UBUS_TIMEOUT, 0x0b2d0000);
    put32(host + PCIE_MISC_RC_CONFIG_RETRY_TIMEOUT, 0x0aba0000);
    put32(host + PCIE_MISC_RC_BAR3_CONFIG_LO, 0);

    /* Gen2, bridge class, outbound CPU 0x1f00000000 -> PCIe address 0. */
    uint32_t lnkcap = get32(host + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCAP);
    put32(host + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCAP, (lnkcap & ~0xfu) | 2);
    uint16_t lnkctl2 = read16(host + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCTL2);
    write16(host + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCTL2, (lnkctl2 & ~0xfu) | 2);
    update32(host + PCIE_RC_CFG_PRIV1_ID_VAL3, 0x00ffffff, 0x00060400);
    put32(host + PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO, 0);
    put32(host + PCIE_MISC_CPU_2_PCIE_MEM_WIN0_HI, 0);
    put32(host + PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT, 0xfff00000);
    put32(host + PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI, 0x1f);
    put32(host + PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI, 0x1f);
    update32(host + PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1, 0x0c, 0);

    /* MIP mapping used by the host bridge even though GT911 does not use MSI. */
    put32(host + PCIE_MISC_RC_BAR1_CONFIG_LO, 0xfffff000u | encode_ibar_size(0x1000));
    put32(host + PCIE_MISC_RC_BAR1_CONFIG_HI, 0xff);
    put32(host + PCIE_MISC_UBUS_BAR1_CONFIG_REMAP, 0x00130000u | 1);
    put32(host + PCIE_MISC_UBUS_BAR1_CONFIG_REMAP_HI, 0x10);

    /* Deassert RP1 PERST# and wait up to one second for the link. */
    update32(host + PCIE_MISC_PCIE_CTRL, 1u << 2, 1u << 2);
    usleep(100000);
    for (int waited = 0; waited < 900 && !link_up(host); waited++)
        usleep(1000);
    return link_up(host) ? 0 : -3;
}

int bcm2712_rp1_init(void) {
    uint32_t page_size = 4096;

    if (rp1_ready)
        return 0;

    if (get_runtime_page_size(&page_size) != 0) {
        klog("rp1-pcie: failed to get runtime page size\n");
        return -1;
    }

    ewokos_addr_t host = _mmio_base + PI5_PCIE2_WIN_OFF;
    ewokos_addr_t ctrl_base = _mmio_base + PI5_RP1_CTRL_WIN_OFF;
    ewokos_addr_t reset = ctrl_base + (PI5_RESET_PAGE_PHY & (page_size - 1u));
    ewokos_addr_t rescal = ctrl_base + page_size +
            (PI5_RESCAL_PAGE_PHY & (page_size - 1u));
    if (map_page_window(host, PI5_PCIE2_PHY, PI5_PCIE2_WIN_SIZE) != 0 ||
        map_subpage_window(reset, PI5_RESET_PAGE_PHY, page_size, &reset) != 0 ||
        map_subpage_window(rescal, PI5_RESCAL_PAGE_PHY, page_size, &rescal) != 0) {
        klog("rp1-pcie: control window mapping failed\n");
        return -1;
    }

    if (!link_up(host)) {
        int ret = train_link(host, reset, rescal);
        if (ret) {
            klog("rp1-pcie: link training failed step=%d status=%08x\n",
                ret, get32(host + PCIE_MISC_PCIE_STATUS));
            return -2;
        }
    }

    /* applied unconditionally: the link may already have been trained
       by another driver process, and these RC-side writes are idempotent */
    set_tc_qos(host);
    /* likewise idempotent, and must hold even when the link was trained
       by firmware with power management already armed */
    lock_link_state(host);

    /* Enable bus 1 and forward its memory transactions through the bridge. */
    write8(host + PCI_CACHE_LINE_SIZE, 64 / 4);
    write8(host + PCI_SECONDARY_BUS, 1);
    write8(host + PCI_SUBORDINATE_BUS, 1);
    write16(host + PCI_MEMORY_BASE, 0);
    write16(host + PCI_MEMORY_LIMIT, 0);
    write16(host + PCI_BRIDGE_CONTROL, PCI_BRIDGE_CTL_PARITY);
    write16(host + PCI_COMMAND, PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER |
        PCI_COMMAND_PARITY | PCI_COMMAND_SERR);

    put32(host + PCIE_EXT_CFG_INDEX, 1u << 20);
    ewokos_addr_t rp1_cfg = host + PCIE_EXT_CFG_DATA;
    uint32_t id = get32(rp1_cfg);
    if (id == 0xffffffff || id == 0xdeaddead) {
        klog("rp1-pcie: configuration space unavailable id=%08x\n", id);
        return -3;
    }
    lock_endpoint_state(rp1_cfg);
    write8(rp1_cfg + PCI_CACHE_LINE_SIZE, 64 / 4);
    put32(rp1_cfg + PCI_BASE_ADDRESS_0, PCI_BASE_ADDRESS_MEM_64);
    put32(rp1_cfg + PCI_BASE_ADDRESS_1, 0);
    put32(rp1_cfg + PCI_BASE_ADDRESS_2, 0x00400000 | PCI_BASE_ADDRESS_MEM_64);
    put32(rp1_cfg + PCI_BASE_ADDRESS_3, 0);
    write16(rp1_cfg + PCI_COMMAND, PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER |
        PCI_COMMAND_PARITY | PCI_COMMAND_SERR);

    rp1_ready = 1;
    return 0;
}
