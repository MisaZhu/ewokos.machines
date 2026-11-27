/*
 * bsp_eth.c: polled RTL8139 Ethernet driver for the x86 (QEMU "-M pc") port.
 *
 * This is the x86 analogue of machine.virt's virtio-net back end: a small
 * hardware library consumed by the /dev/eth0 vdevice driver, which in turn is
 * opened by netd's ether_tap. Like the uhci host controller driver it is fully
 * polled -- no interrupt is taken, the net driver's loop_step reaps the ring.
 *
 * Semantics below match QEMU's hw/net/rtl8139.c ring-Rx / legacy-Tx model:
 *  - RxConfig (0x44) write latches RxBufferSize = 8192 << bits[12:11]; with
 *    those bits 0 the ring is exactly 8192 bytes. The WRAP bit (0x80) is left
 *    clear so QEMU split-writes a straddling frame back to offset 0 instead of
 *    running past the end of the buffer.
 *  - Each received frame is stored as [status:u16][length:u16][frame][crc:u32],
 *    where length = frame_size + 4 (header only, CRC excluded). The read offset
 *    is the RxBufPtr; writing CAPR (0x38) sets RxBufPtr = val + 0x10.
 *  - Legacy Tx: writing TSD[n] (0x10+4n) with a byte count clears the OWN bit
 *    and transmits synchronously from TSAD[n] (0x20+4n); QEMU then re-sets
 *    OWN|TxStatOK. Reset leaves every TSD with OWN set, so the first write must
 *    clear it (a plain length does).
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <bsp/bsp_eth.h>
#include <bsp/x86_pio.h>
#include <ewoksys/dma.h>
#include <ewoksys/proc.h>

#define PCI_CFG_ADDR_PORT 0xCF8
#define PCI_CFG_DATA_PORT 0xCFC

#define PCI_CMD_IO_ENABLE   0x0001
#define PCI_CMD_MEM_ENABLE  0x0002
#define PCI_CMD_BUS_MASTER  0x0004

#define RTL8139_VENDOR 0x10EC
#define RTL8139_DEVICE 0x8139

/* register offsets (I/O space, BAR0) */
#define REG_MAC0      0x00
#define REG_TSD0      0x10
#define REG_TSAD0     0x20
#define REG_RBSTART   0x30
#define REG_CR        0x37
#define REG_CAPR      0x38   /* RxBufPtr: write val -> read ptr = val + 0x10 */
#define REG_RXBUFADDR 0x3A   /* RxBufAddr: hardware write pointer (read-only) */
#define REG_IMR       0x3C
#define REG_ISR       0x3E
#define REG_RCR       0x44

/* command register bits */
#define CR_RST 0x10
#define CR_RE  0x08
#define CR_TE  0x04

/* receive config bits */
#define RCR_AAP  0x01  /* accept all physical (promiscuous) */
#define RCR_APM  0x02  /* accept physical match */
#define RCR_AM   0x04  /* accept multicast */
#define RCR_AB   0x08  /* accept broadcast */

/* tx status bits */
#define TSD_SIZE_MASK 0x1FFF
#define TSD_OWN       0x2000
#define TSD_TOK       0x8000

/* rx status bits */
#define RX_STATUS_OK  0x0001

#define RX_BUF_LEN   8192u               /* must match RCR length bits (0) */
#define RX_BUF_MASK  (RX_BUF_LEN - 1u)
#define RX_BUF_ALLOC (RX_BUF_LEN + 16u)  /* slack; hardware wraps at 8192 */
#define RX_MAX_FRAME 1536u

#define TX_DESC_COUNT 4
#define TX_BUF_SIZE   1536u

#define DMA_TOTAL (RX_BUF_ALLOC + TX_DESC_COUNT * TX_BUF_SIZE)

/* software frame queue: buffers drained off the DMA ring for netd to read */
#define RXQ_SLOTS 32

typedef struct {
    uint16_t len;
    uint8_t  data[RX_MAX_FRAME];
} rx_slot_t;

static bool     _present = false;
static uint16_t _io_base = 0;
static uint8_t  _mac[6];

static ewokos_addr_t _dma_virt = 0;
static uint8_t      *_rx_ring  = NULL;
static uint32_t      _rx_ring_phys = 0;
static uint16_t      _rx_ptr = 0;    /* RxBufPtr: next frame offset in ring */

static uint8_t  *_tx_buf[TX_DESC_COUNT];
static uint32_t  _tx_phys[TX_DESC_COUNT];
static bool      _tx_inflight[TX_DESC_COUNT];
static int       _tx_cur = 0;

static rx_slot_t _rxq[RXQ_SLOTS];
static int       _rxq_head = 0;
static int       _rxq_count = 0;

/* ---------------- pci config space ---------------- */

static uint32_t pci_cfg_addr(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    return 0x80000000u |
            ((uint32_t)bus << 16) |
            ((uint32_t)dev << 11) |
            ((uint32_t)func << 8) |
            (offset & 0xFCu);
}

static uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    x86_outl(PCI_CFG_ADDR_PORT, pci_cfg_addr(bus, dev, func, offset));
    return x86_inl(PCI_CFG_DATA_PORT);
}

static uint16_t pci_cfg_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t value = pci_cfg_read32(bus, dev, func, offset);
    return (uint16_t)((value >> ((offset & 0x2u) * 8u)) & 0xFFFFu);
}

static void pci_cfg_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint16_t value) {
    uint32_t shift = (offset & 0x2u) * 8u;
    uint32_t reg = pci_cfg_read32(bus, dev, func, offset);
    reg &= ~(0xFFFFu << shift);
    reg |= ((uint32_t)value << shift);
    x86_outl(PCI_CFG_ADDR_PORT, pci_cfg_addr(bus, dev, func, offset));
    x86_outl(PCI_CFG_DATA_PORT, reg);
}

static int rtl8139_find(uint16_t *io_base) {
    for (uint8_t dev = 0; dev < 32; ++dev) {
        for (uint8_t func = 0; func < 8; ++func) {
            uint32_t id = pci_cfg_read32(0, dev, func, 0x00);
            uint16_t vendor = (uint16_t)(id & 0xFFFFu);
            uint16_t device = (uint16_t)((id >> 16) & 0xFFFFu);
            uint16_t cmd;
            uint32_t bar0;

            if (vendor == 0xFFFFu) {
                if (func == 0) {
                    break;
                }
                continue;
            }
            if (vendor != RTL8139_VENDOR || device != RTL8139_DEVICE) {
                continue;
            }

            cmd = pci_cfg_read16(0, dev, func, 0x04);
            cmd |= (PCI_CMD_IO_ENABLE | PCI_CMD_MEM_ENABLE | PCI_CMD_BUS_MASTER);
            pci_cfg_write16(0, dev, func, 0x04, cmd);

            bar0 = pci_cfg_read32(0, dev, func, 0x10);
            if ((bar0 & 0x1u) == 0) {   /* BAR0 must be I/O space */
                continue;
            }
            *io_base = (uint16_t)(bar0 & ~0x3u);
            return 0;
        }
    }
    return -1;
}

/* ---------------- rx ring access (handles wraparound) ---------------- */

static inline uint8_t rx_read8(uint16_t off) {
    return _rx_ring[off & RX_BUF_MASK];
}

static inline uint16_t rx_read16(uint16_t off) {
    return (uint16_t)(rx_read8(off) | ((uint16_t)rx_read8((uint16_t)(off + 1u)) << 8));
}

static void rx_copy(uint16_t off, uint8_t *dst, uint16_t len) {
    for (uint16_t i = 0; i < len; ++i) {
        dst[i] = rx_read8((uint16_t)(off + i));
    }
}

/* ---------------- register helpers ---------------- */

static inline uint8_t  reg_r8(uint16_t reg)  { return x86_inb((uint16_t)(_io_base + reg)); }
static inline uint16_t reg_r16(uint16_t reg) { return x86_inw((uint16_t)(_io_base + reg)); }
static inline uint32_t reg_r32(uint16_t reg) { return x86_inl((uint16_t)(_io_base + reg)); }
static inline void reg_w8(uint16_t reg, uint8_t v)   { x86_outb((uint16_t)(_io_base + reg), v); }
static inline void reg_w16(uint16_t reg, uint16_t v) { x86_outw((uint16_t)(_io_base + reg), v); }
static inline void reg_w32(uint16_t reg, uint32_t v) { x86_outl((uint16_t)(_io_base + reg), v); }

/* ---------------- public API ---------------- */

int bsp_eth_init(void) {
    uint32_t m0, m1;
    ewokos_addr_t dma_virt;
    uint32_t dma_phys;
    int i;

    if (_present) {
        return 0;
    }

    if (rtl8139_find(&_io_base) != 0) {
        return -1;
    }

    /* software reset, then wait for the RST bit to self-clear */
    reg_w8(REG_CR, CR_RST);
    for (i = 0; i < 1000; ++i) {
        if ((reg_r8(REG_CR) & CR_RST) == 0) {
            break;
        }
        usleep(1000);
    }
    if (reg_r8(REG_CR) & CR_RST) {
        return -1;
    }

    m0 = reg_r32(REG_MAC0);
    m1 = reg_r32(REG_MAC0 + 4);
    _mac[0] = (uint8_t)(m0 & 0xFF);
    _mac[1] = (uint8_t)((m0 >> 8) & 0xFF);
    _mac[2] = (uint8_t)((m0 >> 16) & 0xFF);
    _mac[3] = (uint8_t)((m0 >> 24) & 0xFF);
    _mac[4] = (uint8_t)(m1 & 0xFF);
    _mac[5] = (uint8_t)((m1 >> 8) & 0xFF);

    dma_virt = dma_alloc(0, DMA_TOTAL);
    if (dma_virt == 0) {
        return -1;
    }
    dma_phys = (uint32_t)dma_phy_addr(0, dma_virt);
    memset((void *)dma_virt, 0, DMA_TOTAL);

    _dma_virt      = dma_virt;
    _rx_ring       = (uint8_t *)dma_virt;
    _rx_ring_phys  = dma_phys;
    for (i = 0; i < TX_DESC_COUNT; ++i) {
        _tx_buf[i]      = (uint8_t *)(dma_virt + RX_BUF_ALLOC + (uint32_t)i * TX_BUF_SIZE);
        _tx_phys[i]     = dma_phys + RX_BUF_ALLOC + (uint32_t)i * TX_BUF_SIZE;
        _tx_inflight[i] = false;
    }
    _rx_ptr    = 0;
    _tx_cur    = 0;
    _rxq_head  = 0;
    _rxq_count = 0;

    /* RxConfig write latches RxBufferSize (8192) and resets RxBufPtr/Addr;
       WRAP left clear so QEMU split-writes straddling frames. Then point the
       chip at our DMA ring. */
    reg_w32(REG_RCR, RCR_AAP | RCR_APM | RCR_AM | RCR_AB);
    reg_w32(REG_RBSTART, _rx_ring_phys);

    /* polled driver: mask all interrupts, clear any latched status */
    reg_w16(REG_IMR, 0x0000);
    reg_w16(REG_ISR, 0xFFFF);

    /* enable receiver + transmitter */
    reg_w8(REG_CR, CR_RE | CR_TE);

    /* align the software read pointer with RxBufPtr=0 (CAPR = ptr - 0x10) */
    reg_w16(REG_CAPR, (uint16_t)((0u - 0x10u) & 0xFFFFu));

    _present = true;
    return 0;
}

int bsp_eth_read_mac(uint8_t mac[6]) {
    if (!_present) {
        return -1;
    }
    memcpy(mac, _mac, 6);
    return 0;
}

void bsp_eth_poll(void) {
    int budget;

    if (!_present) {
        return;
    }

    /* reap completed transmissions so can_write() re-opens the descriptor */
    for (int i = 0; i < TX_DESC_COUNT; ++i) {
        if (_tx_inflight[i]) {
            uint32_t st = reg_r32((uint16_t)(REG_TSD0 + i * 4));
            if ((st & TSD_TOK) != 0 || (st & TSD_OWN) == 0) {
                _tx_inflight[i] = false;
            }
        }
    }

    /* drain the hardware ring into the software frame queue */
    budget = RXQ_SLOTS;
    while (budget-- > 0) {
        uint16_t wr = reg_r16(REG_RXBUFADDR);
        uint16_t status, total, frame_len;
        int slot;

        if (wr == _rx_ptr) {
            break;   /* caught up with the hardware write pointer */
        }

        status   = rx_read16(_rx_ptr);
        total    = rx_read16((uint16_t)(_rx_ptr + 2u));
        if ((status & RX_STATUS_OK) == 0 || total < 4u) {
            break;   /* not a valid frame: stop rather than desync the ring */
        }
        frame_len = (uint16_t)(total - 4u);
        if (frame_len == 0 || frame_len > RX_MAX_FRAME) {
            break;
        }

        if (_rxq_count < RXQ_SLOTS) {
            slot = (_rxq_head + _rxq_count) % RXQ_SLOTS;
            rx_copy((uint16_t)(_rx_ptr + 4u), _rxq[slot].data, frame_len);
            _rxq[slot].len = frame_len;
            _rxq_count++;
        }
        /* else: software queue full, drop the frame but still advance */

        /* consumed = header(4) + frame + crc(4) == total + 4, 4-byte aligned */
        _rx_ptr = (uint16_t)(((_rx_ptr + total + 4u + 3u) & ~3u) & RX_BUF_MASK);
        reg_w16(REG_CAPR, (uint16_t)((_rx_ptr - 0x10u) & 0xFFFFu));
    }
}

int bsp_eth_pending_rx(void) {
    return _present ? _rxq_count : 0;
}

int bsp_eth_can_write(void) {
    if (!_present) {
        return 0;
    }
    return _tx_inflight[_tx_cur] ? 0 : 1;
}

int bsp_eth_read(void *buf, uint32_t size) {
    int slot;
    uint16_t len;

    if (!_present || _rxq_count == 0 || buf == NULL) {
        return 0;
    }
    slot = _rxq_head;
    len  = _rxq[slot].len;
    if ((uint32_t)len > size) {
        len = (uint16_t)size;
    }
    memcpy(buf, _rxq[slot].data, len);
    _rxq_head = (_rxq_head + 1) % RXQ_SLOTS;
    _rxq_count--;
    return (int)len;
}

int bsp_eth_write(const void *buf, uint32_t size) {
    int d;
    int i;

    if (!_present || buf == NULL || size == 0 || size > TX_BUF_SIZE) {
        return 0;
    }
    d = _tx_cur;
    if (_tx_inflight[d]) {
        return 0;   /* descriptor still busy: netd parks on VFS_EVT_WR */
    }

    memcpy(_tx_buf[d], buf, size);
    reg_w32((uint16_t)(REG_TSAD0 + d * 4), _tx_phys[d]);
    /* TSD is a 32-bit register and QEMU only dispatches the legacy-Tx trigger
       on a 4-byte access; a 16-bit write is dropped, leaving the descriptor at
       its reset value (TxHostOwns). Writing the length with OWN clear hands the
       descriptor to the chip and transmits from currTxDesc. */
    reg_w32((uint16_t)(REG_TSD0 + d * 4), (uint32_t)(size & TSD_SIZE_MASK));

    /* QEMU transmits synchronously inside the TSD write; poll briefly so the
       common case frees the descriptor before we advance past it. */
    _tx_inflight[d] = true;
    for (i = 0; i < 100; ++i) {
        uint32_t st = reg_r32((uint16_t)(REG_TSD0 + d * 4));
        if ((st & TSD_TOK) != 0 || (st & TSD_OWN) == 0) {
            _tx_inflight[d] = false;
            break;
        }
        usleep(50);
    }

    _tx_cur = (_tx_cur + 1) % TX_DESC_COUNT;
    return (int)size;
}
