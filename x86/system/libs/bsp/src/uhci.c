/*
 * uhci.c: polled UHCI host controller driver.
 *
 * Carved out of the old monolithic x86 usbd: PCI enumeration, the DMA
 * pool, TD/QH plumbing and the synchronous transfer paths. The policy
 * that used to sit on top (enumeration, HID fan-out) now lives in the
 * shared usbhostd; mass storage rides on the bsp_usb MSC hooks.
 */
#include <bsp/uhci.h>
#include <bsp/x86_pio.h>
#include <stdlib.h>
#include <string.h>
#include <ewoksys/dma.h>
#include <ewoksys/proc.h>

#define PCI_CFG_ADDR_PORT 0xCF8
#define PCI_CFG_DATA_PORT 0xCFC

#define PCI_CLASS_SERIAL_BUS 0x0C
#define PCI_SUBCLASS_USB 0x03
#define PCI_PROGIF_UHCI 0x00
#define PCI_CMD_IO_ENABLE 0x0001
#define PCI_CMD_MEM_ENABLE 0x0002
#define PCI_CMD_BUS_MASTER 0x0004

#define UHCI_DMA_POOL_SIZE 65536
#define UHCI_FRAME_COUNT 1024
#define UHCI_PTR_TERM 0x00000001u
#define UHCI_PTR_QH 0x00000002u
#define UHCI_PTR_DEPTH 0x00000004u

#define UHCI_REG_USBCMD 0x00
#define UHCI_REG_USBSTS 0x02
#define UHCI_REG_USBINTR 0x04
#define UHCI_REG_FRNUM 0x06
#define UHCI_REG_FRBASEADD 0x08
#define UHCI_REG_SOFMOD 0x0C
#define UHCI_REG_PORTSC1 0x10

#define UHCI_CMD_RS 0x0001
#define UHCI_CMD_HCRESET 0x0002
#define UHCI_CMD_CF 0x0040
#define UHCI_CMD_MAXP 0x0080

#define UHCI_PORT_CCS 0x0001
#define UHCI_PORT_CSC 0x0002
#define UHCI_PORT_PE 0x0004
#define UHCI_PORT_PEC 0x0008
#define UHCI_PORT_LSDA 0x0100
#define UHCI_PORT_RESET 0x0200

#define UHCI_TD_STS_ACTIVE 0x00800000u
#define UHCI_TD_STS_STALLED 0x00400000u
#define UHCI_TD_STS_DBUFERR 0x00200000u
#define UHCI_TD_STS_BABBLE 0x00100000u
#define UHCI_TD_STS_NAK 0x00080000u
#define UHCI_TD_STS_TIMEOUT 0x00040000u
#define UHCI_TD_STS_BITSTUFF 0x00020000u
#define UHCI_TD_STS_LS 0x04000000u
#define UHCI_TD_STS_ERRCNT_SHIFT 27
#define UHCI_TD_STS_ERROR_MASK (UHCI_TD_STS_STALLED | UHCI_TD_STS_DBUFERR | \
        UHCI_TD_STS_BABBLE | UHCI_TD_STS_TIMEOUT | UHCI_TD_STS_BITSTUFF)

#define USB_PID_OUT 0xE1
#define USB_PID_IN 0x69
#define USB_PID_SETUP 0x2D

/* one TD per mps chunk keeps buffers off 4K page boundaries */
#define UHCI_MAX_TDS 128
/* interrupt-IN poll: a couple of frames of NAK retry is plenty, the bsp
   pacing owns the cadence */
#define UHCI_INT_IN_TIMEOUT_MS 5u
/* bulk NAK retry budget (flash devices NAK while programming) */
#define UHCI_BULK_NAK_RETRIES 3000

typedef struct __attribute__((packed)) {
    uint32_t link_ptr;
    uint32_t ctrl_status;
    uint32_t token;
    uint32_t buffer_ptr;
} uhci_td_t;

typedef struct __attribute__((packed)) {
    uint32_t head_ptr;
    uint32_t element_ptr;
} uhci_qh_t;

typedef struct {
    ewokos_addr_t virt;
    uint32_t phys;
    uint32_t size;
    uint32_t used;
} dma_pool_t;

typedef struct {
    bool present;
    uint8_t bus;
    uint8_t dev;
    uint8_t func;
    uint16_t io_base;
    uhci_qh_t* async_qh;
    uint32_t async_qh_phys;
    uint32_t* frame_list;
    uint32_t frame_list_phys;
} uhci_ctrl_t;

static dma_pool_t _dma_pool;
static uhci_ctrl_t _ctrls[UHCI_MAX_CONTROLLERS];
static int _ctrl_count = 0;
static bool _inited = false;

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

/* ---------------- dma pool ---------------- */

static inline uint32_t align_up(uint32_t value, uint32_t align) {
    return (value + align - 1u) & ~(align - 1u);
}

static int dma_pool_init(void) {
    _dma_pool.size = UHCI_DMA_POOL_SIZE;
    _dma_pool.used = 0;
    _dma_pool.virt = dma_alloc(0, _dma_pool.size);
    if (_dma_pool.virt == 0) {
        return -1;
    }
    _dma_pool.phys = dma_phy_addr(0, _dma_pool.virt);
    memset((void*)_dma_pool.virt, 0, _dma_pool.size);
    return 0;
}

static void* dma_pool_alloc(uint32_t size, uint32_t align, uint32_t* phys) {
    ewokos_addr_t addr;
    uint32_t aligned;
    if (align == 0) {
        align = 1;
    }
    aligned = align_up(_dma_pool.used, align);
    if ((aligned + size) > _dma_pool.size) {
        return NULL;
    }
    addr = _dma_pool.virt + aligned;
    if (phys != NULL) {
        *phys = _dma_pool.phys + aligned;
    }
    memset((void*)addr, 0, size);
    _dma_pool.used = aligned + size;
    return (void*)addr;
}

static uint32_t dma_pool_mark(void) {
    return _dma_pool.used;
}

static void dma_pool_rewind(uint32_t mark) {
    if (mark <= _dma_pool.size) {
        _dma_pool.used = mark;
    }
}

/* ---------------- registers ---------------- */

static inline uint16_t uhci_readw(const uhci_ctrl_t* hc, uint16_t reg) {
    return x86_inw((uint16_t)(hc->io_base + reg));
}

static inline void uhci_writew(const uhci_ctrl_t* hc, uint16_t reg, uint16_t value) {
    x86_outw((uint16_t)(hc->io_base + reg), value);
}

static inline void uhci_writel(const uhci_ctrl_t* hc, uint16_t reg, uint32_t value) {
    x86_outl((uint16_t)(hc->io_base + reg), value);
}

static inline void uhci_writeb(const uhci_ctrl_t* hc, uint16_t reg, uint8_t value) {
    x86_outb((uint16_t)(hc->io_base + reg), value);
}

static inline uint16_t uhci_port_reg(uint8_t port) {
    return (uint16_t)(UHCI_REG_PORTSC1 + (uint16_t)(port * 2u));
}

static uint16_t uhci_port_read(const uhci_ctrl_t* hc, uint8_t port) {
    return uhci_readw(hc, uhci_port_reg(port));
}

static void uhci_port_write(const uhci_ctrl_t* hc, uint8_t port, uint16_t value) {
    uhci_writew(hc, uhci_port_reg(port), value);
}

/* flat (0-based) port space -> controller + port */
static uhci_ctrl_t* flat_to_ctrl(int flat_port, uint8_t* port) {
    int f = 0;
    if (flat_port < 0) {
        return NULL;
    }
    for (int i = 0; i < UHCI_MAX_CONTROLLERS; ++i) {
        if (!_ctrls[i].present) {
            continue;
        }
        if (flat_port < f + UHCI_PORTS_PER_CTRL) {
            if (port != NULL) {
                *port = (uint8_t)(flat_port - f);
            }
            return &_ctrls[i];
        }
        f += UHCI_PORTS_PER_CTRL;
    }
    return NULL;
}

/* ---------------- td plumbing ---------------- */

static uint32_t uhci_td_token(uint8_t pid, uint8_t addr, uint8_t ep, uint8_t toggle, uint16_t len) {
    uint32_t max_len = (len == 0) ? 0x7FFu : ((uint32_t)len - 1u);
    return (uint32_t)pid |
            ((uint32_t)addr << 8) |
            ((uint32_t)ep << 15) |
            ((uint32_t)toggle << 19) |
            (max_len << 21);
}

static uint32_t uhci_td_status(bool low_speed) {
    uint32_t status = UHCI_TD_STS_ACTIVE | (3u << UHCI_TD_STS_ERRCNT_SHIFT);
    if (low_speed) {
        status |= UHCI_TD_STS_LS;
    }
    return status;
}

static uhci_td_t* uhci_td_alloc(uint32_t* phys) {
    return (uhci_td_t*)dma_pool_alloc(sizeof(uhci_td_t), 16, phys);
}

static uint32_t uhci_link_ptr(uint32_t phys, bool is_qh, bool depth_first) {
    if (phys == UHCI_PTR_TERM) {
        return UHCI_PTR_TERM;
    }
    if (is_qh) {
        return phys | UHCI_PTR_QH;
    }
    return phys | (depth_first ? UHCI_PTR_DEPTH : 0u);
}

static void uhci_td_init(uhci_td_t* td, uint32_t next_ptr, uint8_t pid, uint8_t addr,
        uint8_t ep, uint8_t toggle, uint16_t len, uint32_t buffer_phys, bool low_speed) {
    td->link_ptr = uhci_link_ptr(next_ptr, false, true);
    td->ctrl_status = uhci_td_status(low_speed);
    td->token = uhci_td_token(pid, addr, ep, toggle, len);
    td->buffer_ptr = buffer_phys;
}

static int uhci_td_actual_len(const uhci_td_t* td) {
    uint32_t actual = td->ctrl_status & 0x7FFu;
    return actual == 0x7FFu ? 0 : ((int)actual + 1);
}

static int uhci_wait_chain(uhci_td_t** tds, int td_count, uint32_t timeout_ms) {
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        bool done = true;
        for (int i = 0; i < td_count; ++i) {
            if ((tds[i]->ctrl_status & UHCI_TD_STS_ACTIVE) != 0) {
                done = false;
                break;
            }
        }
        if (done) {
            return 0;
        }
        proc_usleep(1000);
        waited++;
    }
    return -1;
}

static int uhci_run_chain(uhci_ctrl_t* hc, uhci_td_t** tds, uint32_t first_phys, int td_count,
        uint32_t timeout_ms) {
    if (td_count <= 0) {
        return -1;
    }
    hc->async_qh->head_ptr = UHCI_PTR_TERM;
    hc->async_qh->element_ptr = uhci_link_ptr(first_phys, false, true);
    uhci_writew(hc, UHCI_REG_USBSTS, 0xFFFF);
    if (uhci_wait_chain(tds, td_count, timeout_ms) != 0) {
        hc->async_qh->element_ptr = UHCI_PTR_TERM;
        return -1;
    }
    hc->async_qh->element_ptr = UHCI_PTR_TERM;
    return 0;
}

/* a chain that timed out may still carry useful status: a pure NAK
   leaves the TD active with the NAK bit set (no data, not an error) */
static int uhci_chain_timeout_classify(uhci_td_t** tds, int td_count) {
    for (int i = 0; i < td_count; ++i) {
        uint32_t sts = tds[i]->ctrl_status;
        if ((sts & UHCI_TD_STS_ERROR_MASK & ~UHCI_TD_STS_NAK) != 0) {
            return -1;
        }
    }
    return 0; /* only NAKs / still active: no data yet */
}

/* ---------------- transfers ---------------- */

int uhci_control_xfer(int flat_port, bool low_speed, uint8_t addr,
        uint8_t ep_mps, const usb_setup_pkt_t* setup, void* data,
        bool dir_in) {
    uhci_ctrl_t* hc;
    uint32_t mark = dma_pool_mark();
    usb_setup_pkt_t* setup_dma;
    uint32_t setup_phys = 0;
    uint8_t* payload = NULL;
    uint32_t payload_phys = 0;
    uhci_td_t* tds[2 + 512 / 8 + 2];
    uint32_t td_phys[2 + 512 / 8 + 2];
    int td_count = 0;
    uint8_t toggle = 1;
    uint16_t remaining;
    uint32_t cursor_off;
    int bytes_done = 0;
    uint16_t mps = ep_mps == 0 ? 8 : ep_mps;

    hc = flat_to_ctrl(flat_port, NULL);
    if (hc == NULL || setup == NULL) {
        return -1;
    }

    setup_dma = (usb_setup_pkt_t*)dma_pool_alloc(sizeof(usb_setup_pkt_t), 8, &setup_phys);
    if (setup_dma == NULL) {
        return -1;
    }
    memcpy(setup_dma, setup, sizeof(usb_setup_pkt_t));

    if (setup->wLength > 0) {
        payload = (uint8_t*)dma_pool_alloc(setup->wLength, 8, &payload_phys);
        if (payload == NULL) {
            dma_pool_rewind(mark);
            return -1;
        }
        if (!dir_in && data != NULL) {
            memcpy(payload, data, setup->wLength);
        }
    }

    /* setup td + one td per mps chunk + status td */
    remaining = setup->wLength;
    td_count = 1;
    while (remaining > 0) {
        uint16_t chunk = remaining > mps ? mps : remaining;
        remaining = (uint16_t)(remaining - chunk);
        td_count++;
    }
    td_count++;
    if (td_count > (int)(sizeof(tds) / sizeof(tds[0]))) {
        dma_pool_rewind(mark);
        return -1;
    }

    for (int i = 0; i < td_count; ++i) {
        tds[i] = uhci_td_alloc(&td_phys[i]);
        if (tds[i] == NULL) {
            dma_pool_rewind(mark);
            return -1;
        }
    }

    uhci_td_init(tds[0],
            td_count > 1 ? td_phys[1] : UHCI_PTR_TERM,
            USB_PID_SETUP, addr, 0, 0, sizeof(usb_setup_pkt_t), setup_phys, low_speed);

    remaining = setup->wLength;
    toggle = 1;
    cursor_off = 0;
    for (int i = 1; i < td_count - 1; ++i) {
        uint16_t chunk = remaining > mps ? mps : remaining;
        uhci_td_init(tds[i],
                td_phys[i + 1],
                dir_in ? USB_PID_IN : USB_PID_OUT,
                addr, 0, toggle, chunk,
                payload_phys + cursor_off, low_speed);
        cursor_off += chunk;
        remaining = (uint16_t)(remaining - chunk);
        toggle ^= 1u;
    }

    uhci_td_init(tds[td_count - 1],
            UHCI_PTR_TERM,
            dir_in ? USB_PID_OUT : USB_PID_IN,
            addr, 0, 1, 0, 0, low_speed);

    if (uhci_run_chain(hc, tds, td_phys[0], td_count, 200) != 0) {
        dma_pool_rewind(mark);
        return -1;
    }

    for (int i = 0; i < td_count; ++i) {
        if ((tds[i]->ctrl_status & UHCI_TD_STS_ERROR_MASK) != 0 ||
                (tds[i]->ctrl_status & UHCI_TD_STS_NAK) != 0) {
            dma_pool_rewind(mark);
            return -1;
        }
    }

    for (int i = 1; i < td_count - 1; ++i) {
        bytes_done += uhci_td_actual_len(tds[i]);
    }

    if (dir_in && data != NULL && setup->wLength > 0) {
        if (bytes_done > setup->wLength) {
            bytes_done = setup->wLength;
        }
        memcpy(data, payload, bytes_done);
    }
    dma_pool_rewind(mark);
    return bytes_done;
}

/* one data-phase chain (interrupt or bulk): builds n tds of <= mps each
   starting at *toggle. Returns >0 bytes moved, 0 no data, -2 stalled,
   -1 error. Advances *toggle past the moved packets on success. */
static int uhci_data_xfer(uhci_ctrl_t* hc, bool low_speed, bool dir_in,
        uint8_t addr, uint8_t ep, uint16_t mps, uint8_t* toggle,
        void* data, uint32_t len, uint32_t timeout_ms) {
    uint32_t mark = dma_pool_mark();
    uint8_t* payload;
    uint32_t payload_phys = 0;
    uhci_td_t* tds[UHCI_MAX_TDS];
    uint32_t td_phys[UHCI_MAX_TDS];
    int td_count = 0;
    uint8_t tgl;
    uint32_t remaining;
    uint32_t cursor_off;
    int bytes_done = 0;
    int ret;

    if (len == 0 || mps == 0 || len > (uint32_t)UHCI_MAX_TDS * mps) {
        return -1;
    }
    payload = (uint8_t*)dma_pool_alloc(len, 8, &payload_phys);
    if (payload == NULL) {
        return -1;
    }
    if (!dir_in && data != NULL) {
        memcpy(payload, data, len);
    }

    remaining = len;
    while (remaining > 0) {
        remaining -= remaining > mps ? mps : remaining;
        td_count++;
    }
    if (td_count > UHCI_MAX_TDS) {
        dma_pool_rewind(mark);
        return -1;
    }
    for (int i = 0; i < td_count; ++i) {
        tds[i] = uhci_td_alloc(&td_phys[i]);
        if (tds[i] == NULL) {
            dma_pool_rewind(mark);
            return -1;
        }
    }

    tgl = (toggle != NULL) ? *toggle : 0;
    remaining = len;
    cursor_off = 0;
    for (int i = 0; i < td_count; ++i) {
        uint32_t chunk = remaining > mps ? mps : remaining;
        uhci_td_init(tds[i],
                (i + 1 < td_count) ? td_phys[i + 1] : UHCI_PTR_TERM,
                dir_in ? USB_PID_IN : USB_PID_OUT,
                addr, ep, tgl, (uint16_t)chunk,
                payload_phys + cursor_off, low_speed);
        cursor_off += chunk;
        remaining -= chunk;
        tgl ^= 1u;
    }

    if (uhci_run_chain(hc, tds, td_phys[0], td_count, timeout_ms) != 0) {
        ret = uhci_chain_timeout_classify(tds, td_count);
        dma_pool_rewind(mark);
        return ret; /* 0 = pure NAKs (no data), -1 = hard error */
    }

    for (int i = 0; i < td_count; ++i) {
        uint32_t sts = tds[i]->ctrl_status;
        if ((sts & UHCI_TD_STS_STALLED) != 0) {
            dma_pool_rewind(mark);
            return -2;
        }
        if ((sts & UHCI_TD_STS_ERROR_MASK) != 0) {
            dma_pool_rewind(mark);
            return -1;
        }
        if ((sts & UHCI_TD_STS_NAK) != 0) {
            /* NAKed before any packet of this chain moved */
            dma_pool_rewind(mark);
            return 0;
        }
    }

    for (int i = 0; i < td_count; ++i) {
        bytes_done += uhci_td_actual_len(tds[i]);
    }
    if ((uint32_t)bytes_done > len) {
        bytes_done = (int)len;
    }
    if (dir_in && data != NULL && bytes_done > 0) {
        memcpy(data, payload, bytes_done);
    }
    if (toggle != NULL && bytes_done > 0) {
        /* data-phase toggle advances once per moved packet */
        uint32_t packets = ((uint32_t)bytes_done + mps - 1u) / mps;
        *toggle = (uint8_t)((*toggle + packets) & 1u);
    }
    dma_pool_rewind(mark);
    return bytes_done;
}

int uhci_int_in_xfer(int flat_port, bool low_speed, uint8_t addr,
        uint8_t ep, uint16_t mps, uint8_t* toggle, void* data,
        uint16_t size) {
    uint8_t port;
    uhci_ctrl_t* hc = flat_to_ctrl(flat_port, &port);
    if (hc == NULL || size == 0) {
        return -1;
    }
    return uhci_data_xfer(hc, low_speed, true, addr, ep, mps, toggle,
            data, size, UHCI_INT_IN_TIMEOUT_MS);
}

int uhci_bulk_xfer(int flat_port, bool low_speed, bool dir_in,
        uint8_t addr, uint8_t ep, uint16_t mps, uint8_t* toggle,
        void* data, uint32_t len, uint32_t timeout_ms) {
    uint8_t port;
    uhci_ctrl_t* hc = flat_to_ctrl(flat_port, &port);
    int tries = 0;
    int ret;

    if (hc == NULL || len == 0) {
        return -1;
    }
    for (;;) {
        ret = uhci_data_xfer(hc, low_speed, dir_in, addr, ep, mps, toggle,
                data, len, timeout_ms);
        if (ret != 0) {
            return ret;
        }
        /* pure NAK: the device is busy (flash programming, seek...),
           keep the toggle and retry until it moves or errors out */
        if (++tries >= UHCI_BULK_NAK_RETRIES) {
            return -1;
        }
        if ((tries % 50) == 0) {
            proc_usleep(1000);
        }
    }
}

/* ---------------- root ports ---------------- */

int uhci_reset_port(int flat_port) {
    uint8_t port;
    uhci_ctrl_t* hc = flat_to_ctrl(flat_port, &port);
    uint16_t reg;

    if (hc == NULL) {
        return -1;
    }
    uhci_port_write(hc, port, UHCI_PORT_RESET);
    proc_usleep(60000);
    uhci_port_write(hc, port, 0);
    proc_usleep(10000);

    for (int retry = 0; retry < 20; ++retry) {
        reg = uhci_port_read(hc, port);
        uhci_port_write(hc, port, reg | UHCI_PORT_CSC | UHCI_PORT_PEC);
        reg = uhci_port_read(hc, port);
        if ((reg & UHCI_PORT_CCS) == 0) {
            proc_usleep(10000);
            continue;
        }
        reg |= UHCI_PORT_PE;
        uhci_port_write(hc, port, reg | UHCI_PORT_CSC | UHCI_PORT_PEC);
        proc_usleep(10000);
        reg = uhci_port_read(hc, port);
        if ((reg & UHCI_PORT_PE) != 0) {
            return (reg & UHCI_PORT_LSDA) != 0 ? 0 : 1;
        }
    }
    return -1;
}

bool uhci_port_connected(int flat_port) {
    uint8_t port;
    uhci_ctrl_t* hc = flat_to_ctrl(flat_port, &port);
    if (hc == NULL) {
        return false;
    }
    return (uhci_port_read(hc, port) & UHCI_PORT_CCS) != 0;
}

void uhci_ack_port_change(int flat_port) {
    uint8_t port;
    uhci_ctrl_t* hc = flat_to_ctrl(flat_port, &port);
    uint16_t reg;
    if (hc == NULL) {
        return;
    }
    reg = uhci_port_read(hc, port);
    if ((reg & (UHCI_PORT_CSC | UHCI_PORT_PEC)) != 0) {
        uhci_port_write(hc, port, reg | UHCI_PORT_CSC | UHCI_PORT_PEC);
    }
}

void uhci_recover_port(int flat_port) {
    uint8_t port;
    uhci_ctrl_t* hc = flat_to_ctrl(flat_port, &port);
    uint16_t reg;
    if (hc == NULL) {
        return;
    }
    reg = uhci_port_read(hc, port);
    if ((reg & (UHCI_PORT_CSC | UHCI_PORT_PEC)) != 0) {
        uhci_port_write(hc, port, reg | UHCI_PORT_CSC | UHCI_PORT_PEC);
        reg = uhci_port_read(hc, port);
    }
    if ((reg & UHCI_PORT_CCS) != 0 && (reg & UHCI_PORT_PE) == 0) {
        uhci_port_write(hc, port, reg | UHCI_PORT_PE | UHCI_PORT_CSC | UHCI_PORT_PEC);
        proc_usleep(2000);
    }
}

int uhci_port_count(void) {
    int count = 0;
    for (int i = 0; i < UHCI_MAX_CONTROLLERS; ++i) {
        if (_ctrls[i].present) {
            count += UHCI_PORTS_PER_CTRL;
        }
    }
    return count;
}

/* ---------------- bring-up ---------------- */

static int uhci_init_controller(uhci_ctrl_t* hc) {
    if (hc->frame_list == NULL) {
        hc->frame_list = (uint32_t*)dma_pool_alloc(UHCI_FRAME_COUNT * sizeof(uint32_t),
                4096, &hc->frame_list_phys);
        if (hc->frame_list == NULL) {
            return -1;
        }
    }
    if (hc->async_qh == NULL) {
        hc->async_qh = (uhci_qh_t*)dma_pool_alloc(sizeof(uhci_qh_t), 16, &hc->async_qh_phys);
        if (hc->async_qh == NULL) {
            return -1;
        }
    }

    for (int i = 0; i < UHCI_FRAME_COUNT; ++i) {
        hc->frame_list[i] = hc->async_qh_phys | UHCI_PTR_QH;
    }
    hc->async_qh->head_ptr = UHCI_PTR_TERM;
    hc->async_qh->element_ptr = UHCI_PTR_TERM;

    uhci_writew(hc, UHCI_REG_USBCMD, 0);
    proc_usleep(10000);
    uhci_writew(hc, UHCI_REG_USBCMD, UHCI_CMD_HCRESET);
    for (int i = 0; i < 50; ++i) {
        if ((uhci_readw(hc, UHCI_REG_USBCMD) & UHCI_CMD_HCRESET) == 0) {
            break;
        }
        proc_usleep(1000);
    }

    uhci_writew(hc, UHCI_REG_USBSTS, 0xFFFF);
    uhci_writew(hc, UHCI_REG_USBINTR, 0);
    uhci_writew(hc, UHCI_REG_FRNUM, 0);
    uhci_writel(hc, UHCI_REG_FRBASEADD, hc->frame_list_phys);
    uhci_writeb(hc, UHCI_REG_SOFMOD, 0x40);
    uhci_writew(hc, UHCI_REG_USBCMD, UHCI_CMD_RS | UHCI_CMD_CF | UHCI_CMD_MAXP);
    proc_usleep(10000);
    return 0;
}

int uhci_init(void) {
    int count = 0;

    if (_inited) {
        return 0;
    }
    _inited = true;

    if (dma_pool_init() != 0) {
        return 0; /* degrade to zero ports, never fail the daemon */
    }

    for (uint16_t bus = 0; bus < 256 && count < UHCI_MAX_CONTROLLERS; ++bus) {
        for (uint8_t dev = 0; dev < 32 && count < UHCI_MAX_CONTROLLERS; ++dev) {
            for (uint8_t func = 0; func < 8 && count < UHCI_MAX_CONTROLLERS; ++func) {
                uint16_t vendor = pci_cfg_read16((uint8_t)bus, dev, func, 0x00);
                uint32_t class_reg;
                uint8_t class_code;
                uint8_t subclass;
                uint8_t prog_if;
                uint16_t cmd;
                uint16_t io_base = 0;

                if (vendor == 0xFFFF) {
                    if (func == 0) {
                        break;
                    }
                    continue;
                }

                class_reg = pci_cfg_read32((uint8_t)bus, dev, func, 0x08);
                class_code = (uint8_t)(class_reg >> 24);
                subclass = (uint8_t)(class_reg >> 16);
                prog_if = (uint8_t)(class_reg >> 8);
                if (class_code != PCI_CLASS_SERIAL_BUS ||
                        subclass != PCI_SUBCLASS_USB ||
                        prog_if != PCI_PROGIF_UHCI) {
                    continue;
                }

                for (uint8_t bar = 0; bar < 6; ++bar) {
                    uint32_t barv = pci_cfg_read32((uint8_t)bus, dev, func, (uint8_t)(0x10 + bar * 4));
                    if ((barv & 0x1u) != 0 && (barv & ~0x1Fu) != 0) {
                        io_base = (uint16_t)(barv & ~0x1Fu);
                        break;
                    }
                }
                if (io_base == 0) {
                    continue;
                }

                cmd = pci_cfg_read16((uint8_t)bus, dev, func, 0x04);
                cmd |= PCI_CMD_IO_ENABLE | PCI_CMD_MEM_ENABLE | PCI_CMD_BUS_MASTER;
                pci_cfg_write16((uint8_t)bus, dev, func, 0x04, cmd);

                memset(&_ctrls[count], 0, sizeof(_ctrls[count]));
                _ctrls[count].present = true;
                _ctrls[count].bus = (uint8_t)bus;
                _ctrls[count].dev = dev;
                _ctrls[count].func = func;
                _ctrls[count].io_base = io_base;
                if (uhci_init_controller(&_ctrls[count]) == 0) {
                    count++;
                }
                else {
                    memset(&_ctrls[count], 0, sizeof(_ctrls[count]));
                }
            }
        }
    }
    _ctrl_count = count;
    return 0;
}

/* full controller bring-up from scratch after the policy layer gave up
   on a wedged enumeration: HCRESET every present controller and rewind
   the dma pool (all stale TD/QH state is dropped with the devices) */
int uhci_reinit(void) {
    if (!_inited || _dma_pool.virt == 0 || _ctrl_count == 0) {
        return -1;
    }
    _dma_pool.used = 0;
    for (int i = 0; i < UHCI_MAX_CONTROLLERS; ++i) {
        uhci_ctrl_t* hc = &_ctrls[i];
        if (!hc->present) {
            continue;
        }
        /* scratch allocations are gone with the rewind: re-claim them
           before re-running the register bring-up */
        hc->frame_list = NULL;
        hc->async_qh = NULL;
        if (uhci_init_controller(hc) != 0) {
            memset(hc, 0, sizeof(*hc));
        }
    }
    return 0;
}
