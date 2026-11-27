#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <bsp/bsp_eth.h>
#include <bsp/bsp_pcie.h>
#include <ewoksys/syscall.h>
#include <ewoksys/dma.h>
#include <ewoksys/klog.h>
#include <orin_net.h>

/*
 * Realtek RTL8168 gigabit ethernet MAC driver (polled), on Tegra234 PCIe C8.
 *
 * Inherits the UEFI PCIe state: bsp_pcie enumerates the NIC and returns its
 * BAR0 physical address; this driver maps the BAR and drives the MAC.  No
 * BPMP / root-complex / PHY-firmware bring-up.  Register layout from the
 * upstream Linux r8169 driver (ChipCmd=0x37, descriptors {opts1,opts2,addr}).
 *
 * The PHY is internal to the RTL8168 and auto-negotiates; link state is read
 * from PHYstatus (0x6c) so no MDIO/OCP PHY programming is required for basic
 * operation.  DMA buffers come from the kernel DMA32 pool (physical < 4GB).
 */

#define ETH_RING_SIZE   64u
#define ETH_BUF_SIZE    2048u
#define ETH_RXQ_SIZE    64u
#define ETH_DESC_SIZE   16u

#define MAC_VA          0x62000000ULL

/* RTL8168 register offsets (r8169 enum rtl_registers). */
#define R_MAC0          0x00
#define R_TX_DESC_LO    0x20
#define R_TX_DESC_HI    0x24
#define R_CHIPCMD       0x37
#define R_TXPOLL        0x38
#define R_INTRMASK      0x3c
#define R_INTRSTATUS    0x3e
#define R_TXCONFIG      0x40
#define R_RXCONFIG      0x44
#define R_CFG9346       0x50
#define R_PHYSTATUS     0x6c
#define R_RXMAXSIZE     0xda
#define R_CPLUSCMD      0xe0
#define R_RX_DESC_LO    0xe4
#define R_RX_DESC_HI    0xe8
#define R_MAXTXPKT      0xec

/* ChipCmd bits */
#define CMD_RESET       0x10
#define CMD_RXENB       0x08
#define CMD_TXENB       0x04
/* Cfg9346 */
#define CFG_UNLOCK      0xc0
#define CFG_LOCK        0x00
/* TxPoll */
#define NPQ             0x40
/* descriptor opts1 bits */
#define DESC_OWN        0x80000000u
#define DESC_RINGEND    0x40000000u
#define DESC_FIRST      0x20000000u
#define DESC_LAST       0x10000000u
#define DESC_RXLEN_MASK 0x00003fffu
/* RxConfig */
#define RX_FIFO_THRESH  (7u << 13)
#define RX_DMA_BURST    (7u << 8)
#define RX_ACCEPT       0x0eu   /* broadcast | multicast | my-phys */
/* TxConfig */
#define TX_DMA_BURST    (7u << 8)
#define TX_IFG          (3u << 24)
/* CPlusCmd */
#define CP_RXVLAN       0x40
#define CP_RXCHKSUM     0x20
#define PCIDAC          0x10
#define PCIMULRW        0x08
/* PHYstatus */
#define PHY_LINK        0x02
#define PHY_FULLDUP     0x01
#define PHY_1000        0x10
#define PHY_100         0x08
#define PHY_10          0x04

typedef struct {
    uint32_t opts1;
    uint32_t opts2;
    uint64_t addr;
} __attribute__((packed)) rtl_desc_t;

typedef struct {
    uint8_t  data[ETH_BUF_SIZE];
    uint16_t len;
} rxq_slot_t;

static ewokos_addr_t _mac;
static int _present;
static uint8_t _mac_addr[6];

static rtl_desc_t* _tx_desc;
static rtl_desc_t* _rx_desc;
static uint64_t _tx_desc_phy;
static uint64_t _rx_desc_phy;
static uint8_t* _tx_buf[ETH_RING_SIZE];
static uint8_t* _rx_buf[ETH_RING_SIZE];
static uint64_t _tx_buf_phy[ETH_RING_SIZE];
static uint64_t _rx_buf_phy[ETH_RING_SIZE];
static int _tx_busy[ETH_RING_SIZE];
static uint32_t _tx_prod;
static uint32_t _rx_cons;

static rxq_slot_t _rxq[ETH_RXQ_SIZE];
static uint32_t _rxq_head;
static uint32_t _rxq_count;

static uint32_t _rx_packets, _tx_packets, _rx_dropped, _rx_errors, _rxq_full;

static inline uint32_t rd32(uint32_t off) {
    return *(volatile uint32_t*)(uintptr_t)(_mac + off);
}
static inline void wr32(uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(uintptr_t)(_mac + off) = v;
}
static inline uint8_t rd8(uint32_t off) {
    return *(volatile uint8_t*)(uintptr_t)(_mac + off);
}
static inline void wr8(uint32_t off, uint8_t v) {
    *(volatile uint8_t*)(uintptr_t)(_mac + off) = v;
}
static inline uint16_t rd16(uint32_t off) {
    return *(volatile uint16_t*)(uintptr_t)(_mac + off);
}
static inline void wr16(uint32_t off, uint16_t v) {
    *(volatile uint16_t*)(uintptr_t)(_mac + off) = v;
}
static inline void dma_barrier(void) {
    __asm__ volatile("dsb sy" ::: "memory");
}

static int mac_map(uint64_t phys, uint32_t size) {
    ewokos_addr_t ret = syscall3(SYS_MEM_MAP, (ewokos_addr_t)MAC_VA,
            (ewokos_addr_t)phys, (ewokos_addr_t)size);
    if (ret != (ewokos_addr_t)MAC_VA) {
        klog("orin eth: BAR0 map failed phys=%llx\n",
                (unsigned long long)phys);
        return -1;
    }
    _mac = (ewokos_addr_t)MAC_VA;
    return 0;
}

static int dma_alloc_rings(void) {
    uint32_t ring_bytes = ETH_RING_SIZE * ETH_DESC_SIZE;
    uint32_t total = ring_bytes * 2u + ETH_RING_SIZE * ETH_BUF_SIZE * 2u;
    ewokos_addr_t virt = dma_alloc(total);
    if (virt == 0)
        return -1;
    uint64_t phy = (uint64_t)dma_phy_addr(virt);
    if (phy == 0)
        return -1;
    memset((void*)(uintptr_t)virt, 0, total);

    uint32_t off = 0;
    _tx_desc = (rtl_desc_t*)(uintptr_t)(virt + off);
    _tx_desc_phy = phy + off;
    off += ring_bytes;
    _rx_desc = (rtl_desc_t*)(uintptr_t)(virt + off);
    _rx_desc_phy = phy + off;
    off += ring_bytes;
    for (uint32_t i = 0; i < ETH_RING_SIZE; i++) {
        _tx_buf[i] = (uint8_t*)(uintptr_t)(virt + off);
        _tx_buf_phy[i] = phy + off;
        off += ETH_BUF_SIZE;
        _rx_buf[i] = (uint8_t*)(uintptr_t)(virt + off);
        _rx_buf_phy[i] = phy + off;
        off += ETH_BUF_SIZE;
    }
    return 0;
}

static int mac_valid(const uint8_t* m) {
    int all0 = 1, allff = 1;
    for (int i = 0; i < 6; i++) {
        if (m[i] != 0x00) all0 = 0;
        if (m[i] != 0xff) allff = 0;
    }
    return !all0 && !allff && !(m[0] & 0x01); /* unicast */
}

static void mac_reset(void) {
    wr8(R_CFG9346, CFG_UNLOCK);
    wr8(R_CHIPCMD, CMD_RESET);
    for (volatile uint32_t i = 0; i < 1000000u; i++) {
        if ((rd8(R_CHIPCMD) & CMD_RESET) == 0)
            break;
    }
}

int bsp_eth_init(void) {
    orin_pcie_dev_t nic;
    /* C8 is BPMP power-gated after UEFI ExitBootServices, so an inherit ECAM
     * read RAS-faults.  Cold-own the root complex via BPMP (power/clock/reset
     * + P2U + LTSSM), then enumerate the RTL8168. */
    if (bsp_pcie_cold_own(ORIN_PCIE_CTL_C8) != 0) {
        klog("orin eth: PCIe C8 cold-own failed\n");
        return -1;
    }
    if (bsp_pcie_find_device(ORIN_PCIE_CTL_C8, ORIN_RTL8168_VENDOR,
                ORIN_RTL8168_DEVICE, &nic) != 0 &&
            bsp_pcie_find_device(ORIN_PCIE_CTL_C8, ORIN_RTL8168_VENDOR,
                ORIN_RTL8168_DEVICE2, &nic) != 0) {
        klog("orin eth: RTL8168 not found after cold-own\n");
        return -1;
    }
    if (mac_map(nic.bar0_phys, nic.bar0_size) != 0)
        return -1;

    mac_reset();

    /* MAC address: firmware normally programmed it; read IDR0..5. */
    for (int i = 0; i < 6; i++)
        _mac_addr[i] = rd8(R_MAC0 + i);
    if (!mac_valid(_mac_addr)) {
        /* Locally-administered unicast fallback so RX AcceptMyPhys works. */
        const uint8_t def[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
        memcpy(_mac_addr, def, 6);
        for (int i = 0; i < 6; i++)
            wr8(R_MAC0 + i, _mac_addr[i]);
        klog("orin eth: no firmware MAC, using %02x:%02x:%02x:%02x:%02x:%02x\n",
                _mac_addr[0], _mac_addr[1], _mac_addr[2],
                _mac_addr[3], _mac_addr[4], _mac_addr[5]);
    }

    if (dma_alloc_rings() != 0) {
        klog("orin eth: DMA ring alloc failed\n");
        return -1;
    }

    for (uint32_t i = 0; i < ETH_RING_SIZE; i++) {
        uint32_t eor = (i == ETH_RING_SIZE - 1) ? DESC_RINGEND : 0;
        _rx_desc[i].addr = _rx_buf_phy[i];
        _rx_desc[i].opts2 = 0;
        _rx_desc[i].opts1 = DESC_OWN | eor | ETH_BUF_SIZE;
        _tx_desc[i].addr = _tx_buf_phy[i];
        _tx_desc[i].opts2 = 0;
        _tx_desc[i].opts1 = eor;
        _tx_busy[i] = 0;
    }
    dma_barrier();

    wr8(R_CFG9346, CFG_UNLOCK);
    wr32(R_TX_DESC_LO, (uint32_t)_tx_desc_phy);
    wr32(R_TX_DESC_HI, (uint32_t)(_tx_desc_phy >> 32));
    wr32(R_RX_DESC_LO, (uint32_t)_rx_desc_phy);
    wr32(R_RX_DESC_HI, (uint32_t)(_rx_desc_phy >> 32));

    wr32(R_TXCONFIG, TX_DMA_BURST | TX_IFG);
    wr32(R_RXCONFIG, RX_FIFO_THRESH | RX_DMA_BURST | RX_ACCEPT);
    wr16(R_RXMAXSIZE, ETH_BUF_SIZE);
    wr8(R_MAXTXPKT, 8064 >> 7);
    wr16(R_CPLUSCMD, CP_RXVLAN | CP_RXCHKSUM | PCIDAC | PCIMULRW);
    wr16(R_INTRMASK, 0);            /* polled: no interrupts */
    wr16(R_INTRSTATUS, 0xffff);

    wr8(R_CHIPCMD, CMD_RXENB | CMD_TXENB);
    wr8(R_CFG9346, CFG_LOCK);
    dma_barrier();

    _present = 1;
    klog("orin eth: RTL8168 up mac=%02x:%02x:%02x:%02x:%02x:%02x phystatus=%02x\n",
            _mac_addr[0], _mac_addr[1], _mac_addr[2],
            _mac_addr[3], _mac_addr[4], _mac_addr[5], rd8(R_PHYSTATUS));
    return 0;
}

int bsp_eth_read_mac(uint8_t mac[6]) {
    if (!_present)
        return -1;
    memcpy(mac, _mac_addr, 6);
    return 0;
}

void bsp_eth_poll(void) {
    if (!_present)
        return;

    for (uint32_t i = 0; i < ETH_RING_SIZE; i++) {
        if (_tx_busy[i] && (_tx_desc[i].opts1 & DESC_OWN) == 0)
            _tx_busy[i] = 0;
    }

    uint32_t budget = ETH_RING_SIZE;
    while (budget-- > 0) {
        uint32_t i = _rx_cons;
        uint32_t opts1 = _rx_desc[i].opts1;
        if (opts1 & DESC_OWN)
            break;
        dma_barrier();

        uint32_t pkt = opts1 & DESC_RXLEN_MASK;   /* includes 4-byte FCS */
        if (pkt >= 64 && pkt <= ETH_BUF_SIZE) {
            uint32_t len = pkt - 4;
            if (_rxq_count == ETH_RXQ_SIZE) {
                _rxq_full++;
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
                klog("orin eth: rx drop desc=%u opts1=%08x pkt=%u\n",
                        i, opts1, pkt);
        }

        uint32_t eor = (i == ETH_RING_SIZE - 1) ? DESC_RINGEND : 0;
        _rx_desc[i].addr = _rx_buf_phy[i];
        _rx_desc[i].opts2 = 0;
        dma_barrier();
        _rx_desc[i].opts1 = DESC_OWN | eor | ETH_BUF_SIZE;
        dma_barrier();
        _rx_cons = (i + 1u) % ETH_RING_SIZE;
    }

    /* Ack any latched interrupt status (polled mode). */
    uint16_t st = rd16(R_INTRSTATUS);
    if (st != 0)
        wr16(R_INTRSTATUS, st);
}

int bsp_eth_pending_rx(void) {
    return _present ? (int)_rxq_count : 0;
}

int bsp_eth_can_write(void) {
    if (!_present)
        return 0;
    if (_tx_busy[_tx_prod] && (_tx_desc[_tx_prod].opts1 & DESC_OWN) == 0)
        _tx_busy[_tx_prod] = 0;
    return _tx_busy[_tx_prod] ? 0 : 1;
}

int bsp_eth_read(void* buf, uint32_t size) {
    if (!_present || buf == NULL || _rxq_count == 0)
        return 0;
    uint32_t len = _rxq[_rxq_head].len;
    if (len > size)
        len = size;
    memcpy(buf, _rxq[_rxq_head].data, len);
    _rxq_head = (_rxq_head + 1u) % ETH_RXQ_SIZE;
    _rxq_count--;
    return (int)len;
}

int bsp_eth_write(const void* buf, uint32_t size) {
    if (!_present || buf == NULL || size == 0 || size > ETH_BUF_SIZE)
        return 0;
    uint32_t i = _tx_prod;
    if (_tx_busy[i] && (_tx_desc[i].opts1 & DESC_OWN) == 0)
        _tx_busy[i] = 0;
    if (_tx_busy[i])
        return 0;

    memcpy(_tx_buf[i], buf, size);
    _tx_desc[i].addr = _tx_buf_phy[i];
    _tx_desc[i].opts2 = 0;
    dma_barrier();
    uint32_t eor = (i == ETH_RING_SIZE - 1) ? DESC_RINGEND : 0;
    _tx_desc[i].opts1 = DESC_OWN | DESC_FIRST | DESC_LAST | eor |
            (size & 0xffffu);
    dma_barrier();
    _tx_busy[i] = 1;
    _tx_prod = (i + 1u) % ETH_RING_SIZE;
    wr8(R_TXPOLL, NPQ);
    _tx_packets++;
    return (int)size;
}

void bsp_eth_stats(char* buf, uint32_t size) {
    if (!_present) {
        snprintf(buf, size, "not present\n");
        return;
    }
    uint8_t ps = rd8(R_PHYSTATUS);
    const char* speed = (ps & PHY_1000) ? "1000" :
            (ps & PHY_100) ? "100" : (ps & PHY_10) ? "10" : "?";
    snprintf(buf, size,
            "link=%s%s speed=%s phystatus=%02x chipcmd=%02x\n"
            "rx=%u tx=%u dropped=%u errors=%u qfull=%u queued=%u\n"
            "rxcons=%u txprod=%u txconfig=%08x rxconfig=%08x cplus=%04x\n",
            (ps & PHY_LINK) ? "up" : "down",
            (ps & PHY_FULLDUP) ? "/full" : "/half",
            speed, ps, rd8(R_CHIPCMD),
            _rx_packets, _tx_packets, _rx_dropped, _rx_errors,
            _rxq_full, _rxq_count,
            _rx_cons, _tx_prod, rd32(R_TXCONFIG), rd32(R_RXCONFIG),
            rd16(R_CPLUSCMD));
}