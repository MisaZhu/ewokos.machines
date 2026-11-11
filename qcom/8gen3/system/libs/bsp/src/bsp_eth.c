/*
 * Q8B on-board Ethernet discovery.
 *
 * The two 1179:0220 MACs sit behind the TC9563/QPS615 PCIe switch on
 * SC8280XP PCIe3a. The independent pcied host owns PCIe3A power, reset,
 * clocks, link and configuration; this endpoint layer consumes the immutable
 * enumeration snapshot and inspects the pre-assigned BARs. TC9563 uses BAR0
 * for bridge/resource manager registers, BAR2 for SRAM, and BAR4 for the
 * XGMAC3 SFR aperture.
 * PF0 DMA channel 0 is used as a small, fully-polled queue; the driver leaves
 * PCIe/XPCS/PHY state intact.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include <bsp/bsp_eth.h>
#include <bsp/pcie.h>
#include <ewoksys/dma.h>
#include <ewoksys/klog.h>
#include <ewoksys/mmio.h>
#include <ewoksys/syscall.h>
#include <ewoksys/kernel_tic.h>

#define TC956X_BAR0_VA      0x52000000ULL
#define TC956X_BAR2_VA      0x52100000ULL
#define TC956X_BAR4_VA      0x52200000ULL
#define TC956X_BAR0_SIZE    0x00004000u
#define TC956X_BAR2_SIZE    0x00080000u
#define TC956X_BAR4_SIZE    0x00080000u

#define PCI_VENDOR_TOSHIBA  0x1179u
#define PCI_DEVICE_ETH      0x0220u

#define TC956X_RSCMNG_ID            0x00002000u
#define TC956X_RSCMNG_FN_TYPE       (1u << 16)
#define TC956X_RSCMNG_VF_MASK       0x00000f00u
#define TC956X_RSCMNG_PF_MASK       0x0000000fu
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

/* Q8B DTS: I2C12 GPIO0/1, BL24C16 at 0x50, MAC cells 0x9e/0xa4.
 * Read only; preserve pin mux/direction/output state. No QUP clock accesses. */
#define EEPROM_GPIO_VA 0x52300000ULL
static ewokos_addr_t eeprom_pin(unsigned pin) { return EEPROM_GPIO_VA + pin * 0x1000u; }
static void eeprom_line(unsigned pin, bool high) {
    ewokos_addr_t p = eeprom_pin(pin);
    uint32_t cfg = get32(p) & ~((0xfu << 2) | (1u << 9));
    reg_write32(p + 4, get32(p + 4) & ~2u);
    reg_write32(p, cfg | (high ? 0 : (1u << 9)));
    usleep(4);
}
static int eeprom_clock(void) {
    eeprom_line(1, true);
    for (unsigned i = 0; i < 100; i++) {
        if (get32(eeprom_pin(1) + 4) & 1) return 0;
        usleep(2);
    }
    return -1;
}
static int eeprom_start(void) {
    eeprom_line(0, true);
    if (eeprom_clock()) return -1;
    if (!(get32(eeprom_pin(0) + 4) & 1)) return -1;
    eeprom_line(0, false);
    eeprom_line(1, false);
    return 0;
}
static int eeprom_bit(bool high) {
    eeprom_line(0, high);
    if (eeprom_clock()) return -1;
    eeprom_line(1, false);
    return 0;
}
static int eeprom_byte(uint8_t byte) {
    for (unsigned i = 0; i < 8; i++)
        if (eeprom_bit(byte & (0x80u >> i))) return -1;
    eeprom_line(0, true);
    if (eeprom_clock()) return -1;
    int ack = !(get32(eeprom_pin(0) + 4) & 1);
    eeprom_line(1, false);
    return ack ? 0 : -1;
}
static bool valid_mac(const uint8_t mac[6]) {
    uint8_t any = 0;
    for (unsigned i = 0; i < 6; i++) any |= mac[i];
    return any && !(mac[0] & 1);
}
static int tc956x_eeprom_mac(unsigned pf, uint8_t mac[6]) {
    if (syscall3(SYS_MEM_MAP, EEPROM_GPIO_VA, 0x0f100000u, 0x4000) != EEPROM_GPIO_VA)
        return -1;
    uint32_t cfg[2], io[2];
    int ret = -1;
    for (unsigned p = 0; p < 2; p++) {
        cfg[p] = get32(eeprom_pin(p));
        io[p] = get32(eeprom_pin(p) + 4);
    }
    if (eeprom_start() || eeprom_byte(0xa0) ||
            eeprom_byte(pf ? 0xa4 : 0x9e) || eeprom_start() || eeprom_byte(0xa1))
        goto out;
    for (unsigned i = 0; i < 6; i++) {
        uint8_t value = 0;
        eeprom_line(0, true);
        for (unsigned bit = 0; bit < 8; bit++) {
            if (eeprom_clock()) goto out;
            value = (value << 1) | (get32(eeprom_pin(0) + 4) & 1);
            eeprom_line(1, false);
        }
        mac[i] = value;
        if (eeprom_bit(i == 5)) goto out; /* final byte NACK */
    }
    ret = valid_mac(mac) ? 0 : -1;
out:
    eeprom_line(0, false);
    (void)eeprom_clock();
    eeprom_line(0, true); /* STOP */
    for (unsigned p = 0; p < 2; p++) {
        reg_write32(eeprom_pin(p) + 4, io[p] & 2u);
        reg_write32(eeprom_pin(p), cfg[p]);
    }
    return ret;
}

/* The host cold-resets QPS615/TC9563. MAC/PCS clocks can no longer be
 * assumed to survive from UEFI. Match dwmac-tc956x.c:mac_enable/pma_init,
 * touching only the selected MAC's fields, never PCIe/switch clocks. */
static bool tc956x_mac_power_on(unsigned pf, ewokos_addr_t mac_base) {
    ewokos_addr_t clk = TC956X_BAR4_VA + (pf ? 0x100c : 0x1004);
    ewokos_addr_t rst = TC956X_BAR4_VA + (pf ? 0x1010 : 0x1008);
    ewokos_addr_t ctl = TC956X_BAR4_VA + (pf ? 0x1074 : 0x1070);
    uint32_t clocks = (1u << 7) | (1u << 14) | (1u << 31);
    uint32_t resets = (1u << 7) | (1u << 30) | (1u << 31);
    uint32_t before_clk = get32(clk), before_rst = get32(rst);
    klog("q8b eth: PF%u SFR clk=%08x rst=%08x emactl=%08x\n",
            pf, before_clk, before_rst, get32(ctl));
    if (before_clk == 0xffffffffu || before_rst == 0xffffffffu)
        return false;
    if ((before_clk & clocks) == clocks && !(before_rst & resets))
        return true;
    if (pf) clocks |= 1u << 15;
    reg_write32(clk, before_clk | clocks);
    uint32_t value = get32(ctl);
    value = (value & ~0x7fu) | (1u << 8) | (1u << 4) | 4u;
    reg_write32(ctl, value); /* SGMII, maximum 2500 Mbps, PHY clock */
    reg_write32(rst, get32(rst) & ~(1u << 7));
    reg_write32(rst, get32(rst) | (1u << 30));
    ewokos_addr_t pma = mac_base + 0x4000;
    reg_write32(pma + 0x1b8, 0);
    for (unsigned i = 0; i < 5; i++) {
        reg_write32(pma + 0x1080 + i * 0x14, 0);
        reg_write32(pma + 0x1090 + i * 0x14, 0);
        reg_write32(pma + 0x1888 + i * 8, (0xf7u << 9) | (1u << 8) | 4u);
    }
    reg_write32(rst, get32(rst) & ~(1u << 30));
    for (unsigned i = 0; i < 10000; i++) {
        if (get32(ctl) & (1u << 21)) {
            reg_write32(rst, get32(rst) & ~(1u << 31));
            klog("q8b eth: PF%u MAC/PMA clocks ready\n", pf);
            return true;
        }
        usleep(100);
    }
    klog("q8b eth: PF%u PMA init timeout emactl=%08x\n", pf, get32(ctl));
    return false;
}

static bool probe_tc956x_pf(const q8b_pcie_device_t *device) {
	uint64_t bar0, bar2, bar4;
    uint32_t rsc_id, pf, mac_lo, mac_hi, version;
    uint16_t phy_id1 = 0xffffu, phy_id2 = 0xffffu;
    ewokos_addr_t mac_base;

	if (!device)
		return false;
	bar0 = device->bar[0];
	bar2 = device->bar[2];
	bar4 = device->bar[4];
    if (bar0 == 0 || bar2 == 0 || bar4 == 0) {
        slog("q8b eth: %02x:%02x.%u missing TC956x BAR0/2/4\n",
				device->bus, device->device, device->function);
        return false;
    }
	if (q8b_pcie_map_bar(device, 0, TC956X_BAR0_VA, TC956X_BAR0_SIZE) ||
			q8b_pcie_map_bar(device, 2, TC956X_BAR2_VA, TC956X_BAR2_SIZE) ||
			q8b_pcie_map_bar(device, 4, TC956X_BAR4_VA, TC956X_BAR4_SIZE))
        return false;

    klog("q8b eth: BAR maps ready; reading resource manager\n");
    rsc_id = get32(TC956X_BAR0_VA + TC956X_RSCMNG_ID);
    klog("q8b eth: resource manager=%08x\n", rsc_id);
    pf = rsc_id & TC956X_RSCMNG_PF_MASK;
    if ((rsc_id & (TC956X_RSCMNG_FN_TYPE | TC956X_RSCMNG_VF_MASK)) != 0 ||
            pf > 1u) {
		slog("q8b eth: %02x:%02x.%u unsupported resource-manager function\n",
				device->bus, device->device, device->function);
        return false;
    }
    if (pf != 0u) {
		slog("q8b eth: %02x:%02x.%u PF%u left untouched; using PF0\n",
				device->bus, device->device, device->function, pf);
        return false;
    }

    mac_base = TC956X_BAR4_VA +
            (pf == 0u ? TC956X_MAC0_BASE : TC956X_MAC1_BASE);
    if (!tc956x_mac_power_on(pf, mac_base))
        return false;
    version = get32(mac_base + GMAC_VERSION);
    slog("q8b eth: PF%u rsc=%08x chip=%08x clk=%08x rst=%08x macver=%08x\n",
            pf, rsc_id, get32(TC956X_BAR4_VA),
            get32(TC956X_BAR4_VA + 0x1004),
            get32(TC956X_BAR4_VA + 0x1008), version);
    mac_lo = get32(mac_base + GMAC_ADDR0_LOW);
    mac_hi = get32(mac_base + GMAC_ADDR0_HIGH);
    _mac[0] = (uint8_t)mac_lo;
    _mac[1] = (uint8_t)(mac_lo >> 8);
    _mac[2] = (uint8_t)(mac_lo >> 16);
    _mac[3] = (uint8_t)(mac_lo >> 24);
    _mac[4] = (uint8_t)mac_hi;
    _mac[5] = (uint8_t)(mac_hi >> 8);

    if (!valid_mac(_mac) && tc956x_eeprom_mac(pf, _mac)) {
        klog("q8b eth: invalid MAC and EEPROM read failed\n");
        return false;
    }
    if (mdio_read_c22(mac_base, TC956X_PHY_ADDR, 2, &phy_id1) != 0 ||
            mdio_read_c22(mac_base, TC956X_PHY_ADDR, 3, &phy_id2) != 0)
        slog("q8b eth: PF%u mdio phy=%u timeout data=%08x\n",
                pf, TC956X_PHY_ADDR,
                get32(mac_base + GMAC_MDIO_DATA));

    klog("q8b eth: PF%u PHY id=%04x:%04x\n", pf, phy_id1, phy_id2);
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

static uint16_t tc956x_pcs_read(unsigned reg) {
    ewokos_addr_t pcs = _mac_base + 0x3a00;
    reg_write32(pcs + 0x3fc, (0x1f0000u | reg) >> 8);
    return (uint16_t)get32(pcs + (reg & 0xff) * 4);
}
static void tc956x_pcs_write(unsigned reg, uint16_t value) {
    ewokos_addr_t pcs = _mac_base + 0x3a00;
    reg_write32(pcs + 0x3fc, (0x1f0000u | reg) >> 8);
    reg_write32(pcs + (reg & 0xff) * 4, value);
}
static uint16_t _phy_mode = 0xffff;

/* Q8B uses an external QCA8081 PHY. Follow the Linux fix_mac_speed and
 * C37 SGMII PCS setup after PHY negotiation; 2500-Mbps startup settings
 * cannot carry a subsequently negotiated 1000-Mbps link. */
static int tc956x_fix_link(uint16_t phy) {
    if (!(phy & (1u << 11))) return 0;
    unsigned speed = (phy >> 7) & 7;
    uint16_t mode = phy & (0x0380u | (1u << 13));
    if (mode == _phy_mode) return 0;
    unsigned select;
    uint16_t bmcr = phy & (1u << 13) ? 0x100 : 0;
    switch (speed) {
    case 4: select = 4; bmcr |= 0x40; break;
    case 2: select = 5; bmcr |= 0x40; break;
    case 1: select = 6; bmcr |= 0x2000; break;
    case 0: select = 7; break;
    default: return -1;
    }
    ewokos_addr_t rst = TC956X_BAR4_VA + 0x1008;
    ewokos_addr_t ctl = TC956X_BAR4_VA + 0x1070;
    reg_write32(rst, get32(rst) | (1u << 30));
    reg_write32(ctl, (get32(ctl) & ~0x7fu) | 0x110u | select);
    ewokos_addr_t pma = _mac_base + 0x4000;
    reg_write32(pma + 0x1b8, 0);
    for (unsigned i = 0; i < 5; i++) {
        reg_write32(pma + 0x1080 + i * 0x14, 0);
        reg_write32(pma + 0x1090 + i * 0x14, 0);
        reg_write32(pma + 0x1888 + i * 8, (0xf7u << 9) | 0x104u);
    }
    reg_write32(rst, get32(rst) & ~(1u << 30));
    unsigned waited;
    for (waited = 0; waited < 10000; waited++) {
        if (get32(ctl) & (1u << 21)) break;
        usleep(100);
    }
    if (waited == 10000) return -1;
    if (_phy_mode == 0xffff) {
        tc956x_pcs_write(0, 0x8000);
        for (waited = 0; waited < 6000; waited++) {
            if (!(tc956x_pcs_read(0) & 0x8000)) break;
            usleep(100);
        }
        if (waited == 6000) return -1;
    }
    /* xpcs_config_operating_mode(): reset defaults to 10GBASE-R, which
     * hides the MII MMD. Switch to the modal PCS type before accessing the
     * C37 SGMII registers; zero readbacks before this are not valid state. */
    ewokos_addr_t pcs = _mac_base + 0x3a00;
    reg_write32(pcs + 0x3fc, 3u << 8);
    if (get32(pcs + 8u * 4) & 1u)
        reg_write32(pcs + 7u * 4, 4u);
    if (tc956x_pcs_read(2) != 0x7996 || tc956x_pcs_read(3) != 0xced0)
        return -1;
    tc956x_pcs_write(0, tc956x_pcs_read(0) & ~0x1000u);
    tc956x_pcs_write(0x8001, (tc956x_pcs_read(0x8001) & ~0xeu) | 4u);
    tc956x_pcs_write(0x8000, (tc956x_pcs_read(0x8000) & ~0x204u) |
            (speed == 4 ? 4u : 0));
    tc956x_pcs_write(0, bmcr | (speed == 4 ? 0 : 0x1000u));
    uint32_t tx = get32(_mac_base + GMAC_TX_CONFIG);
    reg_write32(_mac_base + GMAC_TX_CONFIG,
            (tx & ~GMAC_TX_SS_MASK) | phy_speed_bits(phy, tx));
    _phy_mode = mode;
    klog("q8b eth: PHY speed=%u duplex=%u PCS ctrl=%04x status=%04x an=%04x emactl=%08x\n",
            speed == 4 ? 2500 : speed == 2 ? 1000 : speed == 1 ? 100 : 10,
            !!(phy & (1u << 13)), tc956x_pcs_read(0), tc956x_pcs_read(1),
            tc956x_pcs_read(0x8002), get32(ctl));
    return 0;
}

static int tc956x_dma_reset(void) {
    uint32_t before = get32(_mac_base + DMA_MODE);
    uint32_t value = before | DMA_MODE_SWR;

    reg_write32(_mac_base + DMA_MODE, value);
    for (uint32_t i = 0; i < 1000000u; i++) {
        value = get32(_mac_base + DMA_MODE);
        if ((value & DMA_MODE_SWR) == 0) {
            return 0;
        }
    }

    slog("q8b eth: dma reset timeout mode=%08x->%08x\n", before, value);
    return -1;
}

static int tc956x_start(void) {
    uint32_t value;
    uint16_t bmsr = 0xffffu, phy_status = 0xffffu;

    if (tc956x_dma_alloc() != 0) {
        slog("q8b eth: DMA allocation failed\n");
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
    reg_write32(_mac_base + GMAC_PACKET_FILTER,
            GMAC_FILTER_RA | GMAC_FILTER_PM | GMAC_FILTER_PR);

    (void)mdio_read_c22(_mac_base, TC956X_PHY_ADDR, 1, &bmsr);
    (void)mdio_read_c22(_mac_base, TC956X_PHY_ADDR, 1, &bmsr);
    (void)mdio_read_c22(_mac_base, TC956X_PHY_ADDR, 0x11, &phy_status);
    if (tc956x_fix_link(phy_status)) {
        klog("q8b eth: PCS link configuration failed\n");
        return -1;
    }
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

    return 0;
}

int bsp_eth_init(void) {
    bool found_mac = false;
	int count;

    if (_probed)
        return _present ? 0 : -1;
    _probed = true;
	count = q8b_pcie_service_get_count();
	if (count < 0) {
		slog("q8b eth: PCIe service unavailable\n");
		return -1;
	}
	for (int i = 0; i < count && !found_mac; i++) {
		q8b_pcie_device_t device;
		if (q8b_pcie_service_get_device((unsigned)i, &device) != 0)
			continue;
		if (device.controller != Q8B_PCIE3A ||
				device.vendor_id != PCI_VENDOR_TOSHIBA ||
				device.device_id != PCI_DEVICE_ETH)
			continue;
		found_mac = true;
		_present = probe_tc956x_pf(&device);
	}

    if (!found_mac)
        slog("q8b eth: no 1179:0220 endpoint; firmware PCIe state absent\n");
    else if (!_present)
        slog("q8b eth: endpoint found but TC956x PF0 validation failed\n");
    else if (tc956x_start() != 0) {
        slog("q8b eth: TC956x channel 0 start failed\n");
        _present = false;
    } else
        slog("q8b eth: TC956x XGMAC3 ready base=%llx mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
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
        } else {
            _rx_dropped++;
            _rx_errors++;
            if (_rx_dropped <= 4u)
                slog("q8b eth: rx drop desc=%u st=%08x len=%u\n",
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
            slog("q8b eth: dma error status=%08x reb=%u txd=%08x:%08x rxd=%08x:%08x rxb=%08x:%08x\n",
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
    uint16_t phy = 0xffff, bmsr = 0xffff;
    (void)mdio_read_c22(_mac_base, TC956X_PHY_ADDR, 1, &bmsr);
    (void)mdio_read_c22(_mac_base, TC956X_PHY_ADDR, 0x11, &phy);
    snprintf(buf, size,
            "phy=%04x bmsr=%04x txcfg=%08x pcs=%04x/%04x/%04x\n"
            "rx=%u tx=%u dropped=%u errors=%u backpressure=%u rbu=%u fatal=%08x\n"
            "cons=%u queued=%u owned=%u next=%08x txprod=%u\n"
            "status=%08x rxctl=%08x cur_rx=%08x tail=%08x ring=%08x\n",
            phy, bmsr, get32(_mac_base + GMAC_TX_CONFIG),
            tc956x_pcs_read(0), tc956x_pcs_read(1), tc956x_pcs_read(0x8002),
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
    return (int)size;
}
