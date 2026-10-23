/*
 * Q8B on-board Ethernet discovery.
 *
 * The two 1179:0220 MACs sit behind the TC9563/QPS615 PCIe switch on
 * SC8280XP PCIe3a. Firmware already trains this hierarchy, as it does the
 * DWC3 hosts used by bsp_usb, so the first milestone deliberately preserves
 * that state: enumerate ECAM, enable memory/mastering on a discovered MAC,
 * and inspect its pre-assigned BARs. TC9563 uses BAR0 for bridge/resource
 * manager registers, BAR2 for SRAM, and BAR4 for the XGMAC3 SFR aperture.
 * PF0 DMA channel 0 is used as a small, fully-polled queue; firmware owns the
 * PCIe/XPCS/PHY bring-up and the driver deliberately leaves that state intact.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include <bsp/bsp_eth.h>
#include <ewoksys/dma.h>
#include <ewoksys/klog.h>
#include <ewoksys/mmio.h>
#include <ewoksys/syscall.h>

#define PCIE_ECAM_PHY       0x480000000ULL
#define PCIE_ECAM_VA        0x51000000ULL
#define PCIE_ECAM_BUSES     8u
#define PCIE_ECAM_MAP_SIZE  (PCIE_ECAM_BUSES << 20)

#define PCIE_MEM32_BUS      0x40000000ULL
#define PCIE_MEM32_PHY      0x490000000ULL
#define PCIE_MEM32_SIZE     0x10000000ULL
#define PCIE_MEM64_PHY      0x4a0000000ULL
#define PCIE_MEM64_SIZE     0x20000000ULL

#define PCIE3A_PARF_PHY     0x01c10000ULL
#define PCIE3A_PARF_VA      0x52300000ULL
#define PCIE3A_PARF_SIZE    0x00003000u
#define PCIE3A_PARF_SID_OFF 0x00000234u
#define PCIE3A_PARF_BDF_CFG 0x00002c00u
#define PCIE3A_PARF_BDF_TAB 0x00002000u

#define TC956X_BAR0_VA      0x52000000ULL
#define TC956X_BAR2_VA      0x52100000ULL
#define TC956X_BAR4_VA      0x52200000ULL
#define TC956X_BAR0_SIZE    0x00010000u
#define TC956X_BAR2_SIZE    0x00080000u
#define TC956X_BAR4_SIZE    0x00080000u

#define PCI_VENDOR_TOSHIBA  0x1179u
#define PCI_DEVICE_TC9563   0x0623u
#define PCI_DEVICE_ETH      0x0220u

#define PCI_COMMAND         0x04u
#define PCI_CLASS_REVISION  0x08u
#define PCI_HEADER_TYPE     0x0eu
#define PCI_BAR0            0x10u
#define PCI_PRIMARY_BUS     0x18u
#define PCI_CMD_MEMORY      0x0002u
#define PCI_CMD_MASTER      0x0004u

#define TC956X_RSCMNG_ID            0x00002000u
#define TC956X_RSCMNG_RSC_ST        0x00002008u
#define TC956X_RSCMNG_FN_TYPE       (1u << 16)
#define TC956X_RSCMNG_VF_MASK       0x00000f00u
#define TC956X_RSCMNG_PF_MASK       0x0000000fu
#define TC956X_SRAM_MAC_ADDR        0x00047000u
#define TC956X_SRAM_INIT_DONE       0x00047054u
#define TC956X_MAC0_BASE            0x00040000u
#define TC956X_MAC1_BASE            0x00048000u

/* BAR0 AXI-to-PCIe translation table used by both eMAC DMA engines. */
#define TC956X_ATR_SLV0_TABLE0       0x00000800u
#define TC956X_ATR_SRC_ADDR_LO       0x00u
#define TC956X_ATR_SRC_ADDR_HI       0x04u
#define TC956X_ATR_TRSL_ADDR_LO      0x08u
#define TC956X_ATR_TRSL_ADDR_HI      0x0cu
#define TC956X_ATR_TRSL_PARAM        0x10u
#define TC956X_ATR_IMPL              (1u << 0)
#define TC956X_ATR_SIZE_64G          ((36u - 1u) << 1)

#define GMAC_VERSION        0x0110u
#define GMAC_HW_FEATURE0    0x011cu
#define GMAC_HW_FEATURE1    0x0120u
#define GMAC_HW_FEATURE2    0x0124u
#define GMAC_HW_FEATURE3    0x0128u
#define GMAC_ADDR0_HIGH     0x0300u
#define GMAC_ADDR0_LOW      0x0304u
#define GMAC_TX_CONFIG      0x0000u
#define GMAC_RX_CONFIG      0x0004u
#define GMAC_PACKET_FILTER  0x0008u
#define GMAC_RXQ_CTRL0      0x00a0u
#define GMAC_INT_EN         0x00b4u
#define GMAC_MDIO_ADDR      0x0200u
#define GMAC_MDIO_DATA      0x0204u
#define GMAC_MDIO_C22P      0x0220u
#define GMAC_MDIO_BUSY      (1u << 22)
#define GMAC_MDIO_SADDR     (1u << 18)
#define GMAC_MDIO_READ      (3u << 16)
#define DMA_MODE            0x3000u
#define DMA_SYS_BUS_MODE    0x3004u
#define MTL_RXQ_DMA_MAP0    0x1030u
#define MTL_TXQ0_OPMODE     0x1100u
#define MTL_RXQ0_OPMODE     0x1140u
#define MTL_Q0_INT_EN       0x1170u
#define DMA_CH0_CONTROL     0x3100u
#define DMA_CH0_TX_CONTROL  0x3104u
#define DMA_CH0_RX_CONTROL  0x3108u
#define DMA_CH0_TXDESC_HI   0x3110u
#define DMA_CH0_TXDESC_LO   0x3114u
#define DMA_CH0_RXDESC_HI   0x3118u
#define DMA_CH0_RXDESC_LO   0x311cu
#define DMA_CH0_TX_TAIL     0x3124u
#define DMA_CH0_RX_TAIL     0x312cu
#define DMA_CH0_TX_CONTROL2 0x3130u
#define DMA_CH0_RX_CONTROL2 0x3134u
#define DMA_CH0_INT_EN      0x3138u
#define DMA_CH0_STATUS      0x3160u

#define GMAC_TX_SS_MASK     (7u << 29)
#define GMAC_TX_SS_2500     (2u << 29)
#define GMAC_TX_SS_1000     (3u << 29)
#define GMAC_TX_SS_100      (4u << 29)
#define GMAC_TX_SS_10       (7u << 29)
#define GMAC_TX_JD          (1u << 16)
#define GMAC_TX_ENABLE      (1u << 0)
#define GMAC_RX_CRC_STRIP   ((1u << 2) | (1u << 1))
#define GMAC_RX_ENABLE      (1u << 0)
#define GMAC_FILTER_RA      (1u << 31)
#define GMAC_FILTER_PM      (1u << 4)
#define GMAC_FILTER_PR      (1u << 0)

#define MTL_TXQ_TQS_MASK    (0x3ffu << 16)
#define MTL_TXQ_ENABLE_MASK (3u << 2)
#define MTL_TXQ_ENABLE_DCB  (2u << 2)
#define MTL_TXQ_STORE_FWD   (1u << 1)
#define MTL_RXQ_RQS_MASK    (0x3ffu << 16)
#define MTL_RXQ_STORE_FWD   (1u << 5)
#define MTL_Q_FIFO_8K       (31u << 16)

#define DMA_MODE_DSPW       (1u << 8)
#define DMA_MODE_INTM       (1u << 12)
#define DMA_MODE_SWR        (1u << 0)
#define DMA_SYS_AAL         (1u << 12)
#define DMA_SYS_EAME        (1u << 11)
#define DMA_CH_PBLX8        (1u << 16)
#define DMA_TX_PBL_MASK     (0x3fu << 16)
#define DMA_TX_PBL_32       (32u << 16)
#define DMA_TX_OSP          (1u << 4)
#define DMA_TX_START        (1u << 0)
#define DMA_RX_PBL_MASK     (0x3fu << 16)
#define DMA_RX_PBL_32       (32u << 16)
#define DMA_RX_RBSZ_MASK    (0x3fffu << 1)
#define DMA_RX_START        (1u << 0)
#define DMA_RX_OWRQ_MASK    (3u << 24)
#define DMA_RX_OWRQ_3       (3u << 24)
#define DMA_RING_LEN_MASK   0x1fffu
#define DMA_STATUS_FATAL    (1u << 12)
#define DMA_STATUS_RX_UNAVAILABLE (1u << 7)

#define DESC_OWN            (1u << 31)
#define DESC_RX_CTXT        (1u << 30)
#define DESC_RX_LD          (1u << 28)
#define DESC_RX_ES          (1u << 15)
#define DESC_RX_LEN_MASK    0x3fffu
#define DESC_TX_FD          (1u << 29)
#define DESC_TX_LD          (1u << 28)

#define TC956X_HOST_ADDR_HI 0x10u
#define ETH_RING_SIZE       32u
#define ETH_BUF_SIZE        2048u
/*
 * A busy management VLAN can carry hundreds of ARP/broadcast frames per
 * second.  The software queue must absorb a burst until netd's next read(),
 * otherwise the unicast reply we actually need (ARP/ICMP) is queued behind
 * the broadcasts and then discarded as "queue full".  128 slots cost
 * 256 KiB and cover several seconds of storm.
 */
#define ETH_RXQ_SIZE        128u
#define ETH_DESC_SIZE       16u

#define TC956X_PHY_ADDR     0x1cu

static bool _probed;
static bool _present;
static uint8_t _mac[6];
static ewokos_addr_t _mac_base;

typedef struct {
    volatile uint32_t des0;
    volatile uint32_t des1;
    volatile uint32_t des2;
    volatile uint32_t des3;
} xgmac_desc_t;

typedef struct {
    uint16_t len;
    uint8_t data[ETH_BUF_SIZE];
} rx_slot_t;

static xgmac_desc_t *_tx_desc;
static xgmac_desc_t *_rx_desc;
static uint64_t _tx_desc_phy;
static uint64_t _rx_desc_phy;
static uint8_t *_tx_buf[ETH_RING_SIZE];
static uint8_t *_rx_buf[ETH_RING_SIZE];
static uint64_t _tx_buf_phy[ETH_RING_SIZE];
static uint64_t _rx_buf_phy[ETH_RING_SIZE];
static bool _tx_busy[ETH_RING_SIZE];
static uint32_t _tx_prod;
static uint32_t _rx_cons;
static rx_slot_t _rxq[ETH_RXQ_SIZE];
static uint32_t _rxq_head;
static uint32_t _rxq_count;
static uint32_t _rx_packets;
static uint32_t _tx_packets;
static uint32_t _rx_dropped;
static uint32_t _last_dma_error;
static uint32_t _rx_unavailable;
static uint32_t _rx_errors;
static uint32_t _rx_queue_full;

static void log_frame_summary(const char *dir, uint32_t packet,
        const uint8_t *frame, uint32_t len) {
    uint16_t type;

    if (packet > 16u || len < 14u)
        return;
    type = ((uint16_t)frame[12] << 8) | frame[13];
    if (type == 0x0806u && len >= 42u) {
        uint16_t op = ((uint16_t)frame[20] << 8) | frame[21];

        klog("q8b eth: %s%u len=%u type=arp op=%u %u.%u.%u.%u -> %u.%u.%u.%u\n",
                dir, packet, len, op,
                frame[28], frame[29], frame[30], frame[31],
                frame[38], frame[39], frame[40], frame[41]);
    } else if (type == 0x0800u && len >= 34u) {
        uint32_t ihl = (uint32_t)(frame[14] & 0x0fu) * 4u;
        uint8_t proto = frame[23];
        int icmp_type = -1;

        if (proto == 1u && ihl >= 20u && len > 14u + ihl)
            icmp_type = frame[14u + ihl];
        klog("q8b eth: %s%u len=%u type=ipv4 proto=%u icmp=%d %u.%u.%u.%u -> %u.%u.%u.%u\n",
                dir, packet, len, proto, icmp_type,
                frame[26], frame[27], frame[28], frame[29],
                frame[30], frame[31], frame[32], frame[33]);
    } else {
        klog("q8b eth: %s%u len=%u type=%04x dst=%02x:%02x:%02x:%02x:%02x:%02x\n",
                dir, packet, len, type, frame[0], frame[1], frame[2],
                frame[3], frame[4], frame[5]);
    }
}

static inline void reg_write32(ewokos_addr_t addr, uint32_t value) {
    put32(addr, value);
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline void dma_barrier(void) {
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline uint32_t tc956x_addr_hi(uint64_t phy) {
    return TC956X_HOST_ADDR_HI | ((uint32_t)(phy >> 32) & 0x0fu);
}

static void desc_set_addr(xgmac_desc_t *desc, uint64_t phy) {
    desc->des0 = (uint32_t)phy;
    desc->des1 = tc956x_addr_hi(phy);
}

static void tc956x_config_host_tamap(void) {
    ewokos_addr_t table = TC956X_BAR0_VA + TC956X_ATR_SLV0_TABLE0;

    klog("q8b eth: tamap0 before src=%08x:%08x trsl=%08x:%08x param=%08x\n",
            get32(table + TC956X_ATR_SRC_ADDR_HI),
            get32(table + TC956X_ATR_SRC_ADDR_LO),
            get32(table + TC956X_ATR_TRSL_ADDR_HI),
            get32(table + TC956X_ATR_TRSL_ADDR_LO),
            get32(table + TC956X_ATR_TRSL_PARAM));

    /*
     * The XGMAC DMA presents host addresses in the 0x10_xxxxxxxx window.
     * Table 0 removes that routing bit and emits the low 36-bit address on
     * PCIe. Program SRC_ADDR_LO last because its implementation bit enables
     * the entry.
     */
    reg_write32(table + TC956X_ATR_SRC_ADDR_LO, 0);
    reg_write32(table + TC956X_ATR_TRSL_PARAM, 0);
    reg_write32(table + TC956X_ATR_TRSL_ADDR_HI, 0);
    reg_write32(table + TC956X_ATR_TRSL_ADDR_LO, 0);
    reg_write32(table + TC956X_ATR_SRC_ADDR_HI, TC956X_HOST_ADDR_HI);
    reg_write32(table + TC956X_ATR_SRC_ADDR_LO,
            TC956X_ATR_SIZE_64G | TC956X_ATR_IMPL);

    klog("q8b eth: tamap0 host=%08x:%08x -> %08x:%08x param=%08x\n",
            get32(table + TC956X_ATR_SRC_ADDR_HI),
            get32(table + TC956X_ATR_SRC_ADDR_LO),
            get32(table + TC956X_ATR_TRSL_ADDR_HI),
            get32(table + TC956X_ATR_TRSL_ADDR_LO),
            get32(table + TC956X_ATR_TRSL_PARAM));
}

static void tc956x_log_pcie_routing(void) {
    ewokos_addr_t parf = PCIE3A_PARF_VA;
    uint32_t entries = 0;

    if (syscall3(SYS_MEM_MAP, parf, PCIE3A_PARF_PHY,
            PCIE3A_PARF_SIZE) != parf) {
        klog("q8b eth: pcie3a PARF map failed\n");
    } else {
        klog("q8b eth: pcie3a sid cfg=%08x off=%08x\n",
                get32(parf + PCIE3A_PARF_BDF_CFG),
                get32(parf + PCIE3A_PARF_SID_OFF));
        for (uint32_t i = 0; i < 256u; i++) {
            uint32_t value = get32(parf + PCIE3A_PARF_BDF_TAB + i * 4u);

            if (value == 0)
                continue;
            if (entries < 16u)
                klog("q8b eth: pcie3a sid[%u]=%08x bdf=%04x sidlo=%02x next=%02x\n",
                        i, value, value >> 16, (value >> 8) & 0xffu,
                        value & 0xffu);
            entries++;
        }
        klog("q8b eth: pcie3a sid entries=%u\n", entries);
    }
}

static bool mdio_wait_idle(ewokos_addr_t mac_base, uint32_t *status) {
    uint32_t value = 0;

    for (uint32_t i = 0; i < 1000000u; i++) {
        value = get32(mac_base + GMAC_MDIO_DATA);
        if ((value & GMAC_MDIO_BUSY) == 0) {
            if (status != NULL)
                *status = value;
            return true;
        }
    }
    if (status != NULL)
        *status = value;
    return false;
}

static int mdio_read_c22(ewokos_addr_t mac_base, uint8_t phy,
        uint8_t reg, uint16_t *value) {
    uint32_t data;

    if (phy >= 32u || reg >= 32u || !mdio_wait_idle(mac_base, &data))
        return -1;
    reg_write32(mac_base + GMAC_MDIO_C22P,
            get32(mac_base + GMAC_MDIO_C22P) | (1u << phy));
    reg_write32(mac_base + GMAC_MDIO_ADDR,
            ((uint32_t)phy << 16) | reg);
    reg_write32(mac_base + GMAC_MDIO_DATA,
            GMAC_MDIO_BUSY | GMAC_MDIO_SADDR | GMAC_MDIO_READ);
    if (!mdio_wait_idle(mac_base, &data))
        return -1;
    *value = (uint16_t)data;
    return 0;
}

static inline ewokos_addr_t cfg_addr(uint8_t bus, uint8_t dev,
        uint8_t fn, uint16_t reg) {
    return PCIE_ECAM_VA + ((ewokos_addr_t)bus << 20) +
            ((ewokos_addr_t)dev << 15) + ((ewokos_addr_t)fn << 12) + reg;
}

static inline uint32_t cfg_read32(uint8_t bus, uint8_t dev,
        uint8_t fn, uint16_t reg) {
    return get32(cfg_addr(bus, dev, fn, reg & ~3u));
}

static inline uint16_t cfg_read16(uint8_t bus, uint8_t dev,
        uint8_t fn, uint16_t reg) {
    volatile uint16_t *p = (volatile uint16_t *)(uintptr_t)
            cfg_addr(bus, dev, fn, reg);
    return *p;
}

static inline void cfg_write16(uint8_t bus, uint8_t dev,
        uint8_t fn, uint16_t reg, uint16_t value) {
    volatile uint16_t *p = (volatile uint16_t *)(uintptr_t)
            cfg_addr(bus, dev, fn, reg);
    *p = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint64_t bar_to_cpu(uint64_t bar) {
    if (bar >= PCIE_MEM32_BUS && bar < PCIE_MEM32_BUS + PCIE_MEM32_SIZE)
        return PCIE_MEM32_PHY + (bar - PCIE_MEM32_BUS);
    if (bar >= PCIE_MEM64_PHY && bar < PCIE_MEM64_PHY + PCIE_MEM64_SIZE)
        return bar;
    return 0;
}

static bool map_bar(const char *name, uint8_t bus, uint8_t dev, uint8_t fn,
        uint64_t bar, ewokos_addr_t va, uint32_t size) {
    uint64_t phy = bar_to_cpu(bar);

    if (phy == 0 || (phy & 0xfffu) != 0) {
        klog("q8b eth: %02x:%02x.%u %s bus=%llx not in outbound memory\n",
                bus, dev, fn, name, (unsigned long long)bar);
        return false;
    }
    if (syscall3(SYS_MEM_MAP, va, (ewokos_addr_t)phy, size) != va) {
        klog("q8b eth: %02x:%02x.%u %s map failed pa=%llx size=%x\n",
                bus, dev, fn, name, (unsigned long long)phy, size);
        return false;
    }
    klog("q8b eth: %02x:%02x.%u %s bus=%llx pa=%llx va=%llx size=%x\n",
            bus, dev, fn, name, (unsigned long long)bar,
            (unsigned long long)phy, (unsigned long long)va, size);
    return true;
}

static uint64_t read_bar(uint8_t bus, uint8_t dev, uint8_t fn,
        uint8_t bar_index) {
    uint16_t reg = PCI_BAR0 + (uint16_t)bar_index * 4u;
    uint32_t low = cfg_read32(bus, dev, fn, reg);
    uint64_t bar;

    if (low == 0 || low == 0xffffffffu || (low & 1u))
        return 0;
    bar = (uint64_t)(low & ~0x0fu);
    if ((low & 0x6u) == 0x4u && bar_index + 1u < 6u)
        bar |= (uint64_t)cfg_read32(bus, dev, fn, reg + 4u) << 32;
    return bar;
}

static void log_bars(uint8_t bus, uint8_t dev, uint8_t fn) {
    for (uint8_t bar_index = 0; bar_index < 6; bar_index++) {
        uint16_t reg = PCI_BAR0 + (uint16_t)bar_index * 4u;
        uint32_t low = cfg_read32(bus, dev, fn, reg);

        if (low != 0 && low != 0xffffffffu && !(low & 1u) &&
                (low & 0x6u) == 0x4u && bar_index + 1u < 6u) {
            uint32_t high = cfg_read32(bus, dev, fn, reg + 4u);
            klog("q8b eth: %02x:%02x.%u BAR%u raw=%08x:%08x\n",
                    bus, dev, fn, bar_index, high, low);
            bar_index++;
        } else {
            klog("q8b eth: %02x:%02x.%u BAR%u raw=%08x\n",
                    bus, dev, fn, bar_index, low);
        }
    }
}

static bool probe_tc956x_pf(uint8_t bus, uint8_t dev, uint8_t fn) {
    uint16_t command = cfg_read16(bus, dev, fn, PCI_COMMAND);
    uint64_t bar0, bar2, bar4;
    uint32_t rsc_id, rsc_st, pf, mac_lo, mac_hi, version;
    uint16_t phy_id1 = 0xffffu, phy_id2 = 0xffffu;
    ewokos_addr_t mac_base;

    command |= PCI_CMD_MEMORY | PCI_CMD_MASTER;
    cfg_write16(bus, dev, fn, PCI_COMMAND, command);
    log_bars(bus, dev, fn);

    bar0 = read_bar(bus, dev, fn, 0);
    bar2 = read_bar(bus, dev, fn, 2);
    bar4 = read_bar(bus, dev, fn, 4);
    if (bar0 == 0 || bar2 == 0 || bar4 == 0) {
        klog("q8b eth: %02x:%02x.%u missing TC956x BAR0/2/4\n",
                bus, dev, fn);
        return false;
    }
    if (!map_bar("BAR0", bus, dev, fn, bar0, TC956X_BAR0_VA,
                TC956X_BAR0_SIZE) ||
            !map_bar("BAR2", bus, dev, fn, bar2, TC956X_BAR2_VA,
                TC956X_BAR2_SIZE) ||
            !map_bar("BAR4", bus, dev, fn, bar4, TC956X_BAR4_VA,
                TC956X_BAR4_SIZE))
        return false;

    rsc_id = get32(TC956X_BAR0_VA + TC956X_RSCMNG_ID);
    rsc_st = get32(TC956X_BAR0_VA + TC956X_RSCMNG_RSC_ST);
    pf = rsc_id & TC956X_RSCMNG_PF_MASK;
    klog("q8b eth: %02x:%02x.%u rsc-id=%08x rsc-st=%08x pf=%u m3-init=%08x\n",
            bus, dev, fn, rsc_id, rsc_st, pf,
            get32(TC956X_BAR2_VA + TC956X_SRAM_INIT_DONE));
    if ((rsc_id & (TC956X_RSCMNG_FN_TYPE | TC956X_RSCMNG_VF_MASK)) != 0 ||
            pf > 1u) {
        klog("q8b eth: %02x:%02x.%u unsupported resource-manager function\n",
                bus, dev, fn);
        return false;
    }
    if (pf != 0u) {
        klog("q8b eth: %02x:%02x.%u PF%u left untouched; using PF0\n",
                bus, dev, fn, pf);
        return false;
    }

    mac_base = TC956X_BAR4_VA +
            (pf == 0u ? TC956X_MAC0_BASE : TC956X_MAC1_BASE);
    version = get32(mac_base + GMAC_VERSION);
    mac_lo = get32(mac_base + GMAC_ADDR0_LOW);
    mac_hi = get32(mac_base + GMAC_ADDR0_HIGH);
    _mac[0] = (uint8_t)mac_lo;
    _mac[1] = (uint8_t)(mac_lo >> 8);
    _mac[2] = (uint8_t)(mac_lo >> 16);
    _mac[3] = (uint8_t)(mac_lo >> 24);
    _mac[4] = (uint8_t)mac_hi;
    _mac[5] = (uint8_t)(mac_hi >> 8);

    klog("q8b eth: PF%u XGMAC3 base=%llx ver=%08x feat=%08x/%08x/%08x/%08x\n",
            pf, (unsigned long long)mac_base, version,
            get32(mac_base + GMAC_HW_FEATURE0),
            get32(mac_base + GMAC_HW_FEATURE1),
            get32(mac_base + GMAC_HW_FEATURE2),
            get32(mac_base + GMAC_HW_FEATURE3));
    klog("q8b eth: PF%u mac-reg=%08x:%08x dma=%08x/%08x mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
            pf, get32(mac_base + GMAC_ADDR0_HIGH),
            get32(mac_base + GMAC_ADDR0_LOW), get32(mac_base + DMA_MODE),
            get32(mac_base + DMA_SYS_BUS_MODE), _mac[0], _mac[1], _mac[2],
            _mac[3], _mac[4], _mac[5]);

    if (mdio_read_c22(mac_base, TC956X_PHY_ADDR, 2, &phy_id1) == 0 &&
            mdio_read_c22(mac_base, TC956X_PHY_ADDR, 3, &phy_id2) == 0)
        klog("q8b eth: PF%u mdio phy=%u id=%04x:%04x\n",
                pf, TC956X_PHY_ADDR, phy_id1, phy_id2);
    else
        klog("q8b eth: PF%u mdio phy=%u timeout data=%08x\n",
                pf, TC956X_PHY_ADDR,
                get32(mac_base + GMAC_MDIO_DATA));

    if (version == 0 || version == 0xffffffffu)
        return false;
    tc956x_config_host_tamap();
    _mac_base = mac_base;
    return true;
}

static int tc956x_dma_alloc(void) {
    ewokos_addr_t virt;
    uint64_t phy;
    uint32_t ring_bytes = ETH_RING_SIZE * sizeof(xgmac_desc_t);
    uint32_t total = ring_bytes * 2u + ETH_RING_SIZE * ETH_BUF_SIZE * 2u;
    uint32_t offset = 0;

    virt = dma_alloc(0, total);
    if (virt == 0)
        return -1;
    phy = (uint64_t)dma_phy_addr(0, virt);
    if (phy == 0)
        return -1;
    memset((void *)(uintptr_t)virt, 0, total);

    _tx_desc = (xgmac_desc_t *)(uintptr_t)(virt + offset);
    _tx_desc_phy = phy + offset;
    offset += ring_bytes;
    _rx_desc = (xgmac_desc_t *)(uintptr_t)(virt + offset);
    _rx_desc_phy = phy + offset;
    offset += ring_bytes;
    for (uint32_t i = 0; i < ETH_RING_SIZE; i++) {
        _tx_buf[i] = (uint8_t *)(uintptr_t)(virt + offset);
        _tx_buf_phy[i] = phy + offset;
        offset += ETH_BUF_SIZE;
        _rx_buf[i] = (uint8_t *)(uintptr_t)(virt + offset);
        _rx_buf_phy[i] = phy + offset;
        offset += ETH_BUF_SIZE;
    }
    klog("q8b eth: dma va=%llx pa=%llx size=%u txd=%llx rxd=%llx hi=%02x\n",
            (unsigned long long)virt, (unsigned long long)phy, total,
            (unsigned long long)_tx_desc_phy,
            (unsigned long long)_rx_desc_phy,
            tc956x_addr_hi(phy));
    return 0;
}

static uint32_t phy_speed_bits(uint16_t phy_status, uint32_t current) {
    uint32_t speed;

    if ((phy_status & (1u << 11)) == 0)
        return current & GMAC_TX_SS_MASK;
    speed = (phy_status >> 7) & 7u;
    if (speed == 4u)
        return GMAC_TX_SS_2500;
    if (speed == 2u)
        return GMAC_TX_SS_1000;
    if (speed == 1u)
        return GMAC_TX_SS_100;
    return GMAC_TX_SS_10;
}

static int tc956x_dma_reset(void) {
    uint32_t before = get32(_mac_base + DMA_MODE);
    uint32_t value = before | DMA_MODE_SWR;

    reg_write32(_mac_base + DMA_MODE, value);
    for (uint32_t i = 0; i < 1000000u; i++) {
        value = get32(_mac_base + DMA_MODE);
        if ((value & DMA_MODE_SWR) == 0) {
            klog("q8b eth: dma reset done mode=%08x->%08x polls=%u\n",
                    before, value, i + 1u);
            return 0;
        }
    }

    klog("q8b eth: dma reset timeout mode=%08x->%08x\n", before, value);
    return -1;
}

static int tc956x_start(void) {
    uint32_t value;
    uint32_t packet_filter;
    uint16_t bmsr = 0xffffu, phy_status = 0xffffu;

    if (tc956x_dma_alloc() != 0) {
        klog("q8b eth: DMA allocation failed\n");
        return -1;
    }

    for (uint32_t i = 0; i < ETH_RING_SIZE; i++) {
        desc_set_addr(&_rx_desc[i], _rx_buf_phy[i]);
        _rx_desc[i].des2 = 0;
        _rx_desc[i].des3 = DESC_OWN;
        _tx_busy[i] = false;
    }
    dma_barrier();

    /* Stop only PF0 channel 0 while its rings are replaced. */
    value = get32(_mac_base + DMA_CH0_TX_CONTROL) & ~DMA_TX_START;
    reg_write32(_mac_base + DMA_CH0_TX_CONTROL, value);
    value = get32(_mac_base + DMA_CH0_RX_CONTROL) & ~DMA_RX_START;
    reg_write32(_mac_base + DMA_CH0_RX_CONTROL, value);

    reg_write32(_mac_base + DMA_CH0_INT_EN, 0);
    reg_write32(_mac_base + MTL_Q0_INT_EN, 0);
    reg_write32(_mac_base + GMAC_INT_EN, 0);
    reg_write32(_mac_base + DMA_CH0_STATUS, 0xffffffffu);

    /* Clear DMA state left by UEFI SNP without resetting PCIe/XPCS/PHY. */
    if (tc956x_dma_reset() != 0)
        return -1;

    value = get32(_mac_base + DMA_MODE);
    value = (value & ~DMA_MODE_DSPW) | DMA_MODE_INTM;
    reg_write32(_mac_base + DMA_MODE, value);
    value = get32(_mac_base + DMA_SYS_BUS_MODE) | DMA_SYS_EAME | DMA_SYS_AAL;
    reg_write32(_mac_base + DMA_SYS_BUS_MODE, value);
    reg_write32(_mac_base + DMA_CH0_CONTROL,
            get32(_mac_base + DMA_CH0_CONTROL) | DMA_CH_PBLX8);

    value = get32(_mac_base + DMA_CH0_TX_CONTROL);
    value &= ~(DMA_TX_PBL_MASK | DMA_TX_START);
    value |= DMA_TX_PBL_32 | DMA_TX_OSP;
    reg_write32(_mac_base + DMA_CH0_TX_CONTROL, value);
    value = get32(_mac_base + DMA_CH0_RX_CONTROL);
    value &= ~(DMA_RX_PBL_MASK | DMA_RX_RBSZ_MASK | DMA_RX_START);
    value |= DMA_RX_PBL_32 | (ETH_BUF_SIZE << 1);
    reg_write32(_mac_base + DMA_CH0_RX_CONTROL, value);

    reg_write32(_mac_base + DMA_CH0_TXDESC_HI,
            tc956x_addr_hi(_tx_desc_phy));
    reg_write32(_mac_base + DMA_CH0_TXDESC_LO, (uint32_t)_tx_desc_phy);
    reg_write32(_mac_base + DMA_CH0_RXDESC_HI,
            tc956x_addr_hi(_rx_desc_phy));
    reg_write32(_mac_base + DMA_CH0_RXDESC_LO, (uint32_t)_rx_desc_phy);
    reg_write32(_mac_base + DMA_CH0_TX_CONTROL2,
            (ETH_RING_SIZE - 1u) & DMA_RING_LEN_MASK);
    value = get32(_mac_base + DMA_CH0_RX_CONTROL2);
    value &= ~(DMA_RX_OWRQ_MASK | DMA_RING_LEN_MASK);
    value |= DMA_RX_OWRQ_3 | (ETH_RING_SIZE - 1u);
    reg_write32(_mac_base + DMA_CH0_RX_CONTROL2, value);
    reg_write32(_mac_base + DMA_CH0_TX_TAIL, (uint32_t)_tx_desc_phy);
    reg_write32(_mac_base + DMA_CH0_RX_TAIL,
            (uint32_t)(_rx_desc_phy + ETH_RING_SIZE * ETH_DESC_SIZE));

    value = get32(_mac_base + MTL_TXQ0_OPMODE);
    value &= ~(MTL_TXQ_TQS_MASK | MTL_TXQ_ENABLE_MASK);
    value |= MTL_Q_FIFO_8K | MTL_TXQ_ENABLE_DCB | MTL_TXQ_STORE_FWD;
    reg_write32(_mac_base + MTL_TXQ0_OPMODE, value);
    value = get32(_mac_base + MTL_RXQ0_OPMODE);
    value &= ~MTL_RXQ_RQS_MASK;
    value |= MTL_Q_FIFO_8K | MTL_RXQ_STORE_FWD;
    reg_write32(_mac_base + MTL_RXQ0_OPMODE, value);
    value = get32(_mac_base + MTL_RXQ_DMA_MAP0) & ~0xffu;
    reg_write32(_mac_base + MTL_RXQ_DMA_MAP0, value);
    value = get32(_mac_base + GMAC_RXQ_CTRL0) & ~3u;
    reg_write32(_mac_base + GMAC_RXQ_CTRL0, value | 2u);

    reg_write32(_mac_base + GMAC_ADDR0_LOW,
            (uint32_t)_mac[0] | ((uint32_t)_mac[1] << 8) |
            ((uint32_t)_mac[2] << 16) | ((uint32_t)_mac[3] << 24));
    reg_write32(_mac_base + GMAC_ADDR0_HIGH,
            (1u << 31) | (uint32_t)_mac[4] | ((uint32_t)_mac[5] << 8));
    /*
     * Do not inherit UEFI's address-filter state.  The TC956x firmware can
     * leave inverse/hash filtering configured for SNP; in that state DHCP
     * broadcasts and multicast arrive while unicast to our own MAC is
     * silently rejected.  Receive-all is intentional during bring-up: netd's
     * Ethernet layer still drops foreign unicast before the IP stack.
     */
    packet_filter = get32(_mac_base + GMAC_PACKET_FILTER);
    reg_write32(_mac_base + GMAC_PACKET_FILTER,
            GMAC_FILTER_RA | GMAC_FILTER_PM | GMAC_FILTER_PR);
    klog("q8b eth: packet filter=%08x->%08x\n", packet_filter,
            get32(_mac_base + GMAC_PACKET_FILTER));

    (void)mdio_read_c22(_mac_base, TC956X_PHY_ADDR, 1, &bmsr);
    (void)mdio_read_c22(_mac_base, TC956X_PHY_ADDR, 1, &bmsr);
    (void)mdio_read_c22(_mac_base, TC956X_PHY_ADDR, 0x11, &phy_status);
    value = get32(_mac_base + GMAC_TX_CONFIG);
    value = (value & ~GMAC_TX_SS_MASK) |
            phy_speed_bits(phy_status, value) | GMAC_TX_JD;
    reg_write32(_mac_base + GMAC_TX_CONFIG, value);
    reg_write32(_mac_base + GMAC_RX_CONFIG,
            get32(_mac_base + GMAC_RX_CONFIG) | GMAC_RX_CRC_STRIP);

    reg_write32(_mac_base + DMA_CH0_RX_CONTROL,
            get32(_mac_base + DMA_CH0_RX_CONTROL) | DMA_RX_START);
    reg_write32(_mac_base + DMA_CH0_TX_CONTROL,
            get32(_mac_base + DMA_CH0_TX_CONTROL) | DMA_TX_START);
    reg_write32(_mac_base + GMAC_RX_CONFIG,
            get32(_mac_base + GMAC_RX_CONFIG) | GMAC_RX_ENABLE);
    reg_write32(_mac_base + GMAC_TX_CONFIG,
            get32(_mac_base + GMAC_TX_CONFIG) | GMAC_TX_ENABLE);

    klog("q8b eth: ch0 started phy-bmsr=%04x status=%04x mac=%08x/%08x mtl=%08x/%08x dma=%08x/%08x/%08x\n",
            bmsr, phy_status, get32(_mac_base + GMAC_TX_CONFIG),
            get32(_mac_base + GMAC_RX_CONFIG),
            get32(_mac_base + MTL_TXQ0_OPMODE),
            get32(_mac_base + MTL_RXQ0_OPMODE),
            get32(_mac_base + DMA_CH0_TX_CONTROL),
            get32(_mac_base + DMA_CH0_RX_CONTROL),
            get32(_mac_base + DMA_CH0_STATUS));
    return 0;
}

int bsp_eth_init(void) {
    bool found_mac = false;

    if (_probed)
        return _present ? 0 : -1;
    _probed = true;

    if (syscall3(SYS_MEM_MAP, PCIE_ECAM_VA, PCIE_ECAM_PHY,
            PCIE_ECAM_MAP_SIZE) != PCIE_ECAM_VA) {
        klog("q8b eth: ECAM map failed pa=%llx size=%u\n",
                (unsigned long long)PCIE_ECAM_PHY, PCIE_ECAM_MAP_SIZE);
        return -1;
    }
    klog("q8b eth: probing PCIe3a ECAM buses 0-%u\n", PCIE_ECAM_BUSES - 1u);
    tc956x_log_pcie_routing();

    for (uint8_t bus = 0; bus < PCIE_ECAM_BUSES; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint32_t id0 = cfg_read32(bus, dev, 0, 0);
            if ((id0 & 0xffffu) == 0xffffu || id0 == 0)
                continue;
            uint8_t max_fn = (cfg_read16(bus, dev, 0, PCI_HEADER_TYPE) &
                    0x80u) ? 8u : 1u;
            for (uint8_t fn = 0; fn < max_fn; fn++) {
                uint32_t id = cfg_read32(bus, dev, fn, 0);
                uint16_t vendor = (uint16_t)id;
                uint16_t device = (uint16_t)(id >> 16);
                if (vendor == 0xffffu || vendor == 0)
                    continue;
                uint32_t class_rev = cfg_read32(bus, dev, fn,
                        PCI_CLASS_REVISION);
                uint32_t buses = cfg_read32(bus, dev, fn, PCI_PRIMARY_BUS);
                uint16_t command = cfg_read16(bus, dev, fn, PCI_COMMAND);
                klog("q8b eth: pci %02x:%02x.%u id=%04x:%04x class=%06x cmd=%04x buses=%06x\n",
                        bus, dev, fn, vendor, device, class_rev >> 8,
                        command, buses & 0xffffffu);

                /*
                 * Linux enables bus mastering recursively on every upstream
                 * bridge before starting a PCI endpoint.  UEFI leaves the
                 * Q8B root port and both TC9563 bridges at command 0x0003,
                 * which blocks XGMAC DMA requests travelling upstream.
                 */
                if ((class_rev >> 8) == 0x060400u) {
                    uint16_t enabled = command |
                            PCI_CMD_MEMORY | PCI_CMD_MASTER;

                    if (enabled != command) {
                        cfg_write16(bus, dev, fn, PCI_COMMAND, enabled);
                        klog("q8b eth: pci %02x:%02x.%u bridge cmd=%04x->%04x\n",
                                bus, dev, fn, command,
                                cfg_read16(bus, dev, fn, PCI_COMMAND));
                    }
                }

                if (vendor == PCI_VENDOR_TOSHIBA &&
                        device == PCI_DEVICE_TC9563)
                    klog("q8b eth: TC9563 switch present at %02x:%02x.%u\n",
                            bus, dev, fn);
                if (vendor == PCI_VENDOR_TOSHIBA &&
                        device == PCI_DEVICE_ETH && !found_mac) {
                    found_mac = true;
                    _present = probe_tc956x_pf(bus, dev, fn);
                }
            }
        }
    }

    if (!found_mac)
        klog("q8b eth: no 1179:0220 endpoint; firmware PCIe state absent\n");
    else if (!_present)
        klog("q8b eth: endpoint found but TC956x PF0 validation failed\n");
    else if (tc956x_start() != 0) {
        klog("q8b eth: TC956x channel 0 start failed\n");
        _present = false;
    } else
        klog("q8b eth: TC956x XGMAC3 ready base=%llx mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
                (unsigned long long)_mac_base, _mac[0], _mac[1], _mac[2],
                _mac[3], _mac[4], _mac[5]);
    return _present ? 0 : -1;
}

int bsp_eth_read_mac(uint8_t mac[6]) {
    if (!_present)
        return -1;
    memcpy(mac, _mac, 6);
    return 0;
}

void bsp_eth_poll(void) {
    uint32_t status;
    uint32_t budget = ETH_RING_SIZE;

    if (!_present)
        return;

    for (uint32_t i = 0; i < ETH_RING_SIZE; i++) {
        if (_tx_busy[i] && (_tx_desc[i].des3 & DESC_OWN) == 0)
            _tx_busy[i] = false;
    }

    while (budget-- > 0) {
        uint32_t i = _rx_cons;
        uint32_t des3 = _rx_desc[i].des3;
        uint32_t len = des3 & DESC_RX_LEN_MASK;

        if (des3 & DESC_OWN)
            break;
        dma_barrier();
        if ((des3 & (DESC_RX_CTXT | DESC_RX_ES | DESC_RX_LD)) == DESC_RX_LD &&
                len > 0 && len <= ETH_BUF_SIZE) {
            /* Keep a completed frame in the DMA ring until the IPC reader
             * frees a software slot. Recycling it here used to discard whole
             * bursts whenever poll() ran ahead of read(). Refilling later
             * advances the tail and resumes an RBU-suspended DMA normally. */
            if (_rxq_count == ETH_RXQ_SIZE) {
                _rx_queue_full++;
                break;
            }
            uint32_t slot = (_rxq_head + _rxq_count) % ETH_RXQ_SIZE;
            memcpy(_rxq[slot].data, _rx_buf[i], len);
            _rxq[slot].len = (uint16_t)len;
            _rxq_count++;
            _rx_packets++;
            log_frame_summary("rx", _rx_packets, _rx_buf[i], len);
        } else {
            _rx_dropped++;
            _rx_errors++;
            if (_rx_dropped <= 4u)
                klog("q8b eth: rx drop desc=%u st=%08x len=%u\n",
                        i, des3, len);
        }

        desc_set_addr(&_rx_desc[i], _rx_buf_phy[i]);
        _rx_desc[i].des2 = 0;
        dma_barrier();
        _rx_desc[i].des3 = DESC_OWN;
        dma_barrier();
        _rx_cons = (i + 1u) % ETH_RING_SIZE;
        reg_write32(_mac_base + DMA_CH0_RX_TAIL,
                (uint32_t)(_rx_desc_phy + _rx_cons * ETH_DESC_SIZE));
    }

    status = get32(_mac_base + DMA_CH0_STATUS);
    if (status != 0) {
        if (status & DMA_STATUS_RX_UNAVAILABLE)
            _rx_unavailable++;
        if ((status & DMA_STATUS_FATAL) && status != _last_dma_error) {
            _last_dma_error = status;
            klog("q8b eth: dma error status=%08x reb=%u txd=%08x:%08x rxd=%08x:%08x rxb=%08x:%08x\n",
                    status, (status >> 19) & 7u,
                    get32(_mac_base + 0x3140u),
                    get32(_mac_base + 0x3144u),
                    get32(_mac_base + 0x3148u),
                    get32(_mac_base + 0x314cu),
                    get32(_mac_base + 0x3158u),
                    get32(_mac_base + 0x315cu));
        }
        reg_write32(_mac_base + DMA_CH0_STATUS, status);
    }
}

int bsp_eth_pending_rx(void) {
    return _present ? (int)_rxq_count : 0;
}

void bsp_eth_stats(char *buf, uint32_t size) {
    uint32_t owned = 0;
    if (!_present) {
        snprintf(buf, size, "not present\n");
        return;
    }
    for (uint32_t i = 0; i < ETH_RING_SIZE; i++)
        owned += (_rx_desc[i].des3 & DESC_OWN) != 0;
    snprintf(buf, size,
            "rx=%u tx=%u dropped=%u errors=%u backpressure=%u rbu=%u fatal=%08x\n"
            "cons=%u queued=%u owned=%u next=%08x txprod=%u\n"
            "status=%08x rxctl=%08x cur_rx=%08x tail=%08x ring=%08x\n",
            _rx_packets, _tx_packets, _rx_dropped, _rx_errors,
            _rx_queue_full, _rx_unavailable, _last_dma_error,
            _rx_cons, _rxq_count, owned, _rx_desc[_rx_cons].des3, _tx_prod,
            get32(_mac_base + DMA_CH0_STATUS),
            get32(_mac_base + DMA_CH0_RX_CONTROL),
            get32(_mac_base + 0x314cu),
            get32(_mac_base + DMA_CH0_RX_TAIL), (uint32_t)_rx_desc_phy);
}

int bsp_eth_can_write(void) {
    if (!_present)
        return 0;
    if (_tx_busy[_tx_prod] && (_tx_desc[_tx_prod].des3 & DESC_OWN) == 0)
        _tx_busy[_tx_prod] = false;
    return _tx_busy[_tx_prod] ? 0 : 1;
}

int bsp_eth_read(void *buf, uint32_t size) {
    uint32_t len;

    if (!_present || buf == NULL || _rxq_count == 0)
        return 0;
    len = _rxq[_rxq_head].len;
    if (len > size)
        len = size;
    memcpy(buf, _rxq[_rxq_head].data, len);
    _rxq_head = (_rxq_head + 1u) % ETH_RXQ_SIZE;
    _rxq_count--;
    return (int)len;
}

int bsp_eth_write(const void *buf, uint32_t size) {
    uint32_t i;

    if (!_present || buf == NULL || size == 0 || size > ETH_BUF_SIZE)
        return 0;
    i = _tx_prod;
    if (_tx_busy[i] && (_tx_desc[i].des3 & DESC_OWN) == 0)
        _tx_busy[i] = false;
    if (_tx_busy[i])
        return 0;

    memcpy(_tx_buf[i], buf, size);
    desc_set_addr(&_tx_desc[i], _tx_buf_phy[i]);
    _tx_desc[i].des2 = size;
    dma_barrier();
    _tx_desc[i].des3 = DESC_OWN | DESC_TX_FD | DESC_TX_LD | size;
    dma_barrier();
    _tx_busy[i] = true;
    _tx_prod = (i + 1u) % ETH_RING_SIZE;
    reg_write32(_mac_base + DMA_CH0_TX_TAIL,
            (uint32_t)(_tx_desc_phy + _tx_prod * ETH_DESC_SIZE));
    _tx_packets++;
    log_frame_summary("tx", _tx_packets, buf, size);
    return (int)size;
}
