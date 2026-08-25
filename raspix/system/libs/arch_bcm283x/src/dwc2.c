/*
 * dwc2.c: polled DWC2 OTG host controller driver for BCM283x/BCM2711.
 *
 * Moved out of usbhostd so the bsp usb layer can serve the shared
 * usbhostd policy on raspix. The register programming is the battle-tested
 * sequence from the old monolithic driver: mailbox power-on, UTMI 8-bit
 * PHY selection (USBTRDTIM=9), per-stage control retries, toggle resync
 * from HCTSIZ.PID and the FS/LS-only latch for boards with a dead HS
 * data path.
 */
#include <arch/bcm283x/dwc2.h>
#include <arch/bcm283x/mailbox.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <ewoksys/mmio.h>
#include <ewoksys/dma.h>
#include <ewoksys/proc.h>
#include <ewoksys/klog.h>

#define USB_CORE_OFFSET 0x0980000u

#define USB_DMA_POOL_SIZE 65536u

/* per-stage control transfer timeouts. They only bound the "device is
   completely silent" case: NAK/error handshakes wake the channel via
   hcint immediately. 3 attempts per stage still apply. */
#define USB_CTRL_SETUP_TIMEOUT_MS 100u
#define USB_CTRL_DATA_TIMEOUT_MS 250u
#define USB_CTRL_STATUS_TIMEOUT_MS 100u
#define USB_INT_IN_TIMEOUT_MS 50u

/* channel assignments: control, HID interrupt-IN, MSC bulk out/in */
#define DWC_CH_CTRL 0
#define DWC_CH_INT_IN 1
#define DWC_CH_BULK_OUT 2
#define DWC_CH_BULK_IN 3

/* DWC2 FIFO size registers use 4-byte words, not bytes.
   BCM2835 host mode is known to be stable with the platform defaults. */
#define DWC_RX_FIFO_SIZE 774u
#define DWC_NP_TX_FIFO_SIZE 256u
#define DWC_P_TX_FIFO_SIZE 512u

#define DWC_REG_GOTGCTL 0x000
#define DWC_REG_GAHBCFG 0x008
#define DWC_REG_GUSBCFG 0x00C
#define DWC_REG_GRSTCTL 0x010
#define DWC_REG_GINTSTS 0x014
#define DWC_REG_GINTMSK 0x018
#define DWC_REG_GRXFSIZ 0x024
#define DWC_REG_GNPTXFSIZ 0x028
#define DWC_REG_GSNPSID 0x040
#define DWC_REG_GHWCFG1 0x044
#define DWC_REG_GHWCFG2 0x048
#define DWC_REG_GHWCFG3 0x04C
#define DWC_REG_GHWCFG4 0x050
#define DWC_REG_HPTXFSIZ 0x100
#define DWC_REG_HCFG 0x400
#define DWC_REG_HFIR 0x404
#define DWC_REG_HFNUM 0x408
#define DWC_REG_HAINT 0x414
#define DWC_REG_HAINTMSK 0x418
#define DWC_REG_HPRT 0x440
#define DWC_REG_PCGCR 0xE00

#define DWC_HC_OFFSET(ch, reg) (0x500u + ((uint32_t)(ch) * 0x20u) + (reg))
#define DWC_HCCHAR(ch) DWC_HC_OFFSET(ch, 0x00)
#define DWC_HCSPLT(ch) DWC_HC_OFFSET(ch, 0x04)
#define DWC_HCINT(ch) DWC_HC_OFFSET(ch, 0x08)
#define DWC_HCINTMSK(ch) DWC_HC_OFFSET(ch, 0x0C)
#define DWC_HCTSIZ(ch) DWC_HC_OFFSET(ch, 0x10)
#define DWC_HCDMA(ch) DWC_HC_OFFSET(ch, 0x14)

#define DWC_GAHBCFG_GLBL_INTR_EN (1u << 0)
#define DWC_GAHBCFG_WAIT_AXI_WRITES (1u << 4)
#define DWC_GAHBCFG_DMA_EN (1u << 5)

#define DWC_GOTGCTL_HSTSETHNPEN (1u << 10)
#define DWC_GOTGCTL_CONID_B (1u << 16)

#define DWC_GINTSTS_CURMODE_HOST (1u << 0)

#define DWC_GUSBCFG_PHYIF (1u << 3)
#define DWC_GUSBCFG_ULPI_UTMI_SEL (1u << 4)
#define DWC_GUSBCFG_SRPCAP (1u << 8)
#define DWC_GUSBCFG_HNPCAP (1u << 9)
#define DWC_GUSBCFG_PHY_LP_CLK_SEL (1u << 15)
#define DWC_GUSBCFG_ULPI_FSLS (1u << 17)
#define DWC_GUSBCFG_ULPI_DRV_EXT_VBUS (1u << 20)
#define DWC_GUSBCFG_TSDLINEPULSE (1u << 22)
#define DWC_GUSBCFG_ULPI_CLK_SUSP_M (1u << 19)
#define DWC_GUSBCFG_USBTRDTIM_SHIFT 10
#define DWC_GUSBCFG_USBTRDTIM_MASK (0xFu << DWC_GUSBCFG_USBTRDTIM_SHIFT)
#define DWC_GUSBCFG_FORCE_HOST_MODE (1u << 29)
#define DWC_GUSBCFG_FORCE_DEV_MODE (1u << 30)

#define DWC_GRSTCTL_CSFTRST (1u << 0)
#define DWC_GRSTCTL_RXFFLSH (1u << 4)
#define DWC_GRSTCTL_TXFFLSH (1u << 5)
#define DWC_GRSTCTL_TXFNUM_SHIFT 6
#define DWC_GRSTCTL_AHB_IDLE (1u << 31)

#define DWC_HCFG_FSLSPCLKSEL_30_60MHZ 0x0u
#define DWC_HCFG_FSLSPCLKSEL_48MHZ 0x1u
#define DWC_HCFG_FSLSSUPP (1u << 2)

#define DWC_HFIR_60MHZ_FSLS 59999u
#define DWC_HFIR_48MHZ_FSLS 47999u

#define DWC_HPRT_CONNDET (1u << 1)
#define DWC_HPRT_ENA (1u << 2)
#define DWC_HPRT_ENCHNG (1u << 3)
#define DWC_HPRT_OVRCURRCHNG (1u << 5)
#define DWC_HPRT_RST (1u << 8)
#define DWC_HPRT_PWR (1u << 12)
#define DWC_HPRT_SPEED_SHIFT 17
#define DWC_HPRT_SPEED_MASK (3u << DWC_HPRT_SPEED_SHIFT)

#define DWC_HCCHAR_EPNUM_SHIFT 11
#define DWC_HCCHAR_EPDIR_IN (1u << 15)
#define DWC_HCCHAR_LSPDDEV (1u << 17)
#define DWC_HCCHAR_EPTYPE_SHIFT 18
#define DWC_HCCHAR_MC_SHIFT 20
#define DWC_HCCHAR_DEVADDR_SHIFT 22
#define DWC_HCCHAR_ODDFRM (1u << 29)
#define DWC_HCCHAR_CHDIS (1u << 30)
#define DWC_HCCHAR_CHENA (1u << 31)

#define DWC_HCTSIZ_XFERSIZE_MASK 0x7FFFFu
#define DWC_HCTSIZ_PKTCNT_SHIFT 19
#define DWC_HCTSIZ_PID_SHIFT 29

#define DWC_PID_DATA0 0u
#define DWC_PID_DATA2 1u
#define DWC_PID_DATA1 2u
#define DWC_PID_SETUP 3u

#define DWC_HCINT_XFRC (1u << 0)
#define DWC_HCINT_CHH (1u << 1)
#define DWC_HCINT_AHBERR (1u << 2)
#define DWC_HCINT_STALL (1u << 3)
#define DWC_HCINT_NAK (1u << 4)
#define DWC_HCINT_ACK (1u << 5)
#define DWC_HCINT_NYET (1u << 6)
#define DWC_HCINT_TXERR (1u << 7)
#define DWC_HCINT_BBLERR (1u << 8)
#define DWC_HCINT_FRMOVRUN (1u << 9)
#define DWC_HCINT_DTERR (1u << 10)

/* BCM283x/BCM2711 uses an internal 8-bit UTMI PHY. Linux programs
   USBTRDTIM=9 for this configuration so EP0 setup/data handshakes have
   enough turnaround margin at FS/LS. */
#define DWC_USBTRDTIM_UTMI_8BIT 9u

#define BCM2835_MBOX_TAG_SET_POWER_STATE 0x00028001u
#define BCM2835_MBOX_SET_POWER_STATE_REQ_ON (1u << 0)
#define BCM2835_MBOX_SET_POWER_STATE_REQ_WAIT (1u << 1)
#define BCM2835_MBOX_POWER_DEVID_USB_HCD 3u
#define MAILBOX_VC_ALIAS_NONCACHED 0x40000000u
#define DMA_VC_ALIAS_UNCACHED 0xC0000000u
#define DMA_BUS_ADDR_MASK 0x3FFFFFFFu

typedef struct __attribute__((packed)) {
    uint32_t buf_size;
    uint32_t code;
} bcm2835_mbox_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t tag;
    uint32_t val_buf_size;
    uint32_t val_len;
} bcm2835_mbox_tag_hdr_t;

typedef struct __attribute__((packed)) {
    bcm2835_mbox_tag_hdr_t tag_hdr;
    union {
        struct {
            uint32_t device_id;
            uint32_t state;
        } req;
        struct {
            uint32_t device_id;
            uint32_t state;
        } resp;
    } body;
} bcm2835_mbox_tag_set_power_state_t;

typedef struct __attribute__((packed)) {
    bcm2835_mbox_hdr_t hdr;
    bcm2835_mbox_tag_set_power_state_t set_power_state;
    uint32_t end_tag;
} bcm2835_mbox_power_msg_t;

typedef struct {
    ewokos_addr_t virt;
    uint32_t phys;
    uint32_t size;
    uint32_t used;
} dma_pool_t;

static dma_pool_t _dma_pool;
static ewokos_addr_t _usb_base = 0;
static bool _ready = false;
static uint32_t _last_hcint = 0;
static uint32_t _num_host_channels = 8;
/* latched after a high-speed root-port probe dies with pure TXERR: makes
   subsequent port resets arm FS/LS-only mode (FSLSSUPP) before the reset
   handshake, so the device never chirps and enumerates at full speed */
static bool _force_fs_only = false;

/* The main loop (HID polling, enumeration) and the IPC thread (mass
   storage) both touch the channels and the shared DMA pool, so every
   top-level transfer takes this lock. */
static pthread_mutex_t _xfer_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---------------- DMA pool (bump allocator with mark/rewind) ---------------- */

static inline uint32_t align_up(uint32_t value, uint32_t align) {
    return (value + align - 1u) & ~(align - 1u);
}

static inline uint32_t dwc_dma_bus_addr(uint32_t phys) {
    return (phys & DMA_BUS_ADDR_MASK) | DMA_VC_ALIAS_UNCACHED;
}

static int dma_pool_init(void) {
    _dma_pool.size = USB_DMA_POOL_SIZE;
    _dma_pool.used = 0;
    _dma_pool.virt = dma_alloc(0, _dma_pool.size);
    if (_dma_pool.virt == 0) {
        klog("dwc2: dma alloc_failed size=%u\n", _dma_pool.size);
        return -1;
    }
    _dma_pool.phys = dma_phy_addr(0, _dma_pool.virt);
    memset((void*)(uintptr_t)_dma_pool.virt, 0, _dma_pool.size);
    return 0;
}

static uint32_t dma_pool_mark(void) {
    return _dma_pool.used;
}

static void dma_pool_rewind(uint32_t mark) {
    if (mark <= _dma_pool.size) {
        _dma_pool.used = mark;
    }
}

static void* dma_pool_alloc(uint32_t size, uint32_t align, uint32_t* phys) {
    uint32_t used;
    ewokos_addr_t virt;

    if (align == 0) {
        align = 1;
    }
    used = align_up(_dma_pool.used, align);
    if (used + size > _dma_pool.size) {
        return NULL;
    }
    virt = _dma_pool.virt + used;
    if (phys != NULL) {
        *phys = _dma_pool.phys + used;
    }
    memset((void*)(uintptr_t)virt, 0, size);
    _dma_pool.used = used + size;
    return (void*)(uintptr_t)virt;
}

/* ---------------- mailbox power-on ---------------- */

static int bcm2835_power_on_usb(void) {
    mail_message_t msg;
    bcm2835_mbox_power_msg_t* req;
    uint32_t phy;

    req = (bcm2835_mbox_power_msg_t*)(uintptr_t)dma_alloc(0, sizeof(*req));
    if (req == NULL) {
        return -1;
    }

    memset(req, 0, sizeof(*req));
    req->hdr.buf_size = sizeof(*req);
    req->set_power_state.tag_hdr.tag = BCM2835_MBOX_TAG_SET_POWER_STATE;
    req->set_power_state.tag_hdr.val_buf_size = sizeof(req->set_power_state.body);
    req->set_power_state.tag_hdr.val_len = sizeof(req->set_power_state.body.req);
    req->set_power_state.body.req.device_id = BCM2835_MBOX_POWER_DEVID_USB_HCD;
    req->set_power_state.body.req.state =
            BCM2835_MBOX_SET_POWER_STATE_REQ_ON |
            BCM2835_MBOX_SET_POWER_STATE_REQ_WAIT;

    phy = dma_phy_addr(0, (ewokos_addr_t)(uintptr_t)req);
    msg.data = (phy + MAILBOX_VC_ALIAS_NONCACHED) >> 4;
    msg.channel = PROPERTY_CHANNEL;
    bcm283x_mailbox_call(&msg);
    return 0;
}

/* ---------------- register access ---------------- */

static inline uint32_t usb_readl(uint32_t reg) {
    return *((volatile uint32_t*)(uintptr_t)(_usb_base + reg));
}

static inline void usb_writel(uint32_t reg, uint32_t value) {
    *((volatile uint32_t*)(uintptr_t)(_usb_base + reg)) = value;
}

static inline void dwc_writel_sync(uint32_t reg, uint32_t value) {
    usb_writel(reg, value);
    (void)usb_readl(reg);
}

static int dwc_wait_grstctl_clear(uint32_t mask, uint32_t timeout_ms) {
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        if ((usb_readl(DWC_REG_GRSTCTL) & mask) == 0) {
            return 0;
        }
        proc_usleep(1000);
        waited++;
    }
    return -1;
}

static int dwc_wait_ahb_idle(uint32_t timeout_ms) {
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        if ((usb_readl(DWC_REG_GRSTCTL) & DWC_GRSTCTL_AHB_IDLE) != 0) {
            return 0;
        }
        proc_usleep(1000);
        waited++;
    }
    return -1;
}

static int dwc_core_soft_reset(void) {
    uint32_t reg;
    if (dwc_wait_ahb_idle(100) != 0) {
        return -1;
    }
    reg = usb_readl(DWC_REG_GRSTCTL);
    usb_writel(DWC_REG_GRSTCTL, reg | DWC_GRSTCTL_CSFTRST);
    if (dwc_wait_grstctl_clear(DWC_GRSTCTL_CSFTRST, 100) != 0) {
        return -1;
    }
    /* Some DWC2 revisions need extra settle time before post-reset
       register writes become reliable. */
    proc_usleep(10000);
    return 0;
}

static int dwc_flush_fifos(void) {
    uint32_t reg;
    reg = DWC_GRSTCTL_RXFFLSH;
    usb_writel(DWC_REG_GRSTCTL, reg);
    if (dwc_wait_grstctl_clear(DWC_GRSTCTL_RXFFLSH, 100) != 0) {
        return -1;
    }

    reg = DWC_GRSTCTL_TXFFLSH | (0x10u << DWC_GRSTCTL_TXFNUM_SHIFT);
    usb_writel(DWC_REG_GRSTCTL, reg);
    return dwc_wait_grstctl_clear(DWC_GRSTCTL_TXFFLSH, 100);
}

static int dwc_wait_channel_stopped(int ch, uint32_t timeout_ms) {
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        uint32_t hcchar = usb_readl(DWC_HCCHAR(ch));
        uint32_t hcint = usb_readl(DWC_HCINT(ch));
        if ((hcchar & DWC_HCCHAR_CHENA) == 0 || (hcint & DWC_HCINT_CHH) != 0) {
            return 0;
        }
        proc_usleep(1000);
        waited++;
    }
    return -1;
}

static int dwc_channel_halt(int ch, uint32_t timeout_ms) {
    uint32_t hcchar = usb_readl(DWC_HCCHAR(ch));
    if ((hcchar & (DWC_HCCHAR_CHENA | DWC_HCCHAR_CHDIS)) == 0) {
        usb_writel(DWC_HCINT(ch), 0xFFFFFFFFu);
        return 0;
    }
    usb_writel(DWC_HCCHAR(ch), hcchar | DWC_HCCHAR_CHDIS | DWC_HCCHAR_CHENA);
    if (dwc_wait_channel_stopped(ch, timeout_ms) != 0) {
        return -1;
    }
    usb_writel(DWC_HCINT(ch), 0xFFFFFFFFu);
    usb_writel(DWC_HCINTMSK(ch), 0);
    usb_writel(DWC_HCSPLT(ch), 0);
    usb_writel(DWC_HCTSIZ(ch), 0);
    usb_writel(DWC_HCDMA(ch), 0);
    usb_writel(DWC_HCCHAR(ch), 0);
    return 0;
}

static void dwc_channel_reset_regs(int ch) {
    dwc_writel_sync(DWC_HCINT(ch), 0xFFFFFFFFu);
    dwc_writel_sync(DWC_HCINTMSK(ch), 0);
    dwc_writel_sync(DWC_HCSPLT(ch), 0);
    dwc_writel_sync(DWC_HCTSIZ(ch), 0);
    dwc_writel_sync(DWC_HCDMA(ch), 0);
    dwc_writel_sync(DWC_HCCHAR(ch), 0);
}

static int dwc_host_halt_all_channels(void) {
    uint32_t ch;
    bool port_enabled = (usb_readl(DWC_REG_HPRT) & DWC_HPRT_ENA) != 0;

    for (ch = 0; ch < _num_host_channels; ch++) {
        uint32_t hcchar = DWC_HCCHAR_CHDIS | DWC_HCCHAR_EPDIR_IN;
        usb_writel(DWC_HCINT(ch), 0xFFFFFFFFu);
        usb_writel(DWC_HCINTMSK(ch), 0);
        usb_writel(DWC_HCCHAR(ch), hcchar);
    }

    for (ch = 0; ch < _num_host_channels; ch++) {
        uint32_t waited = 0;
        uint32_t hcchar;

        /* without an enabled port there is no PHY clock, so the
           enable+halt handshake can never complete: just clear regs */
        if (!port_enabled) {
            dwc_channel_reset_regs(ch);
            continue;
        }
        hcchar = usb_readl(DWC_HCCHAR(ch));
        hcchar |= DWC_HCCHAR_CHDIS | DWC_HCCHAR_CHENA | DWC_HCCHAR_EPDIR_IN;
        usb_writel(DWC_HCCHAR(ch), hcchar);
        while ((usb_readl(DWC_HCCHAR(ch)) & DWC_HCCHAR_CHENA) != 0) {
            if (waited++ > 100) {
                break;
            }
            proc_usleep(1000);
        }
        dwc_channel_reset_regs(ch);
    }
    return 0;
}

static void dwc_channel_prepare(int ch) {
    if ((usb_readl(DWC_HCCHAR(ch)) & (DWC_HCCHAR_CHENA | DWC_HCCHAR_CHDIS)) != 0) {
        (void)dwc_channel_halt(ch, 20);
    }
    dwc_channel_reset_regs(ch);
}

/* ---------------- root port ---------------- */

static void dwc_port_write(uint32_t set_bits, uint32_t clear_bits) {
    uint32_t reg = usb_readl(DWC_REG_HPRT);
    reg &= ~(DWC_HPRT_ENA | DWC_HPRT_CONNDET | DWC_HPRT_ENCHNG | DWC_HPRT_OVRCURRCHNG);
    reg &= ~clear_bits;
    reg |= set_bits;
    usb_writel(DWC_REG_HPRT, reg);
}

bool dwc2_port_connected(void) {
    if (!_ready) {
        return false;
    }
    return (usb_readl(DWC_REG_HPRT) & 0x1u) != 0;
}

void dwc2_ack_port_change(void) {
    uint32_t reg;
    uint32_t ack = 0;

    if (!_ready) {
        return;
    }
    reg = usb_readl(DWC_REG_HPRT);
    if (reg & DWC_HPRT_CONNDET) {
        ack |= DWC_HPRT_CONNDET;
    }
    if (reg & DWC_HPRT_ENCHNG) {
        ack |= DWC_HPRT_ENCHNG;
    }
    if (reg & DWC_HPRT_OVRCURRCHNG) {
        ack |= DWC_HPRT_OVRCURRCHNG;
    }
    if (ack != 0) {
        dwc_port_write(ack, 0);
    }
}

int dwc2_reset_port(void) {
    uint32_t reg;
    uint32_t waited_ms = 0;
    bool low_speed;
    uint32_t speed_bits;

    if (!_ready) {
        return -1;
    }
    dwc_port_write(DWC_HPRT_PWR, 0);
    proc_usleep(10000);
    if (!dwc2_port_connected()) {
        return -1;
    }

    /* the HS chirp handshake runs during the reset window, so FS/LS-only
       mode must be armed before reset is asserted: with FSLSSUPP set the
       host never answers the device chirp and the device stays full speed */
    if (_force_fs_only) {
        usb_writel(DWC_REG_HCFG, DWC_HCFG_FSLSPCLKSEL_30_60MHZ | DWC_HCFG_FSLSSUPP);
    }

    dwc_port_write(DWC_HPRT_PWR | DWC_HPRT_RST, 0);
    proc_usleep(30000);
    dwc_port_write(DWC_HPRT_PWR, DWC_HPRT_RST);
    proc_usleep(5000);
    for (;;) {
        reg = usb_readl(DWC_REG_HPRT);
        if ((reg & 0x1u) == 0) {
            return -1;
        }
        if ((reg & DWC_HPRT_ENA) != 0) {
            break;
        }
        if (waited_ms++ >= 50u) {
            dwc2_ack_port_change();
            return -1;
        }
        proc_usleep(1000);
    }
    dwc2_ack_port_change();
    /* USB spec reset recovery: device may ignore traffic briefly after reset.
       10ms is the spec minimum; slow or marginal MCUs (and devices running
       near their brown-out limit) need longer before their transceiver is
       stable, so give them 50ms */
    proc_usleep(50000);

    reg = usb_readl(DWC_REG_HPRT);
    if (!dwc2_port_connected()) {
        return -1;
    }
    speed_bits = (reg & DWC_HPRT_SPEED_MASK) >> DWC_HPRT_SPEED_SHIFT;
    low_speed = speed_bits == 2u;
    /* BCM2835 uses the internal HS UTMI PHY. FS uses 30/60MHz. LS keeps
       the dedicated low-power clock at 48MHz on this platform. */
    {
        uint32_t hcfg = low_speed ?
                (DWC_HCFG_FSLSPCLKSEL_48MHZ | DWC_HCFG_FSLSSUPP) :
                DWC_HCFG_FSLSPCLKSEL_30_60MHZ;
        if (_force_fs_only) {
            hcfg |= DWC_HCFG_FSLSSUPP;
        }
        usb_writel(DWC_REG_HCFG, hcfg);
        usb_writel(DWC_REG_HFIR, low_speed ?
                DWC_HFIR_48MHZ_FSLS :
                DWC_HFIR_60MHZ_FSLS);
    }
    if (speed_bits == 0u) {
        return DWC2_SPEED_HIGH;
    }
    return low_speed ? DWC2_SPEED_LOW : DWC2_SPEED_FULL;
}

void dwc2_force_fs_only(bool enable) {
    _force_fs_only = enable;
}

bool dwc2_last_xfer_txerr(void) {
    return (_last_hcint & DWC_HCINT_TXERR) != 0;
}

/* ---------------- host init ---------------- */

static int dwc_host_init(void) {
    uint32_t reg;

    if (bcm2835_power_on_usb() != 0) {
        klog("dwc2: host init power_on_failed\n");
        return -1;
    }
    proc_usleep(20000);

    /* GHWCFG2[17:14] = number of host channels - 1 */
    _num_host_channels = ((usb_readl(DWC_REG_GHWCFG2) >> 14) & 0xFu) + 1u;

    usb_writel(DWC_REG_PCGCR, 0);
    reg = usb_readl(DWC_REG_GUSBCFG);
    /* Internal PHY on BCM283x/BCM2711 is UTMI+ 8-bit: clear ULPI_UTMI_SEL
       and PHYIF (Linux dwc2 params_bcm2835 does the same). Selecting ULPI
       here leaves the PHY unclocked on BCM2711 (no connect detect). */
    reg &= ~(DWC_GUSBCFG_PHYIF | DWC_GUSBCFG_ULPI_UTMI_SEL |
            DWC_GUSBCFG_SRPCAP | DWC_GUSBCFG_HNPCAP |
            DWC_GUSBCFG_PHY_LP_CLK_SEL | DWC_GUSBCFG_ULPI_FSLS | DWC_GUSBCFG_ULPI_CLK_SUSP_M |
            DWC_GUSBCFG_ULPI_DRV_EXT_VBUS | DWC_GUSBCFG_TSDLINEPULSE |
            DWC_GUSBCFG_FORCE_DEV_MODE | DWC_GUSBCFG_USBTRDTIM_MASK);
    reg |= (DWC_USBTRDTIM_UTMI_8BIT << DWC_GUSBCFG_USBTRDTIM_SHIFT);
    reg |= DWC_GUSBCFG_FORCE_HOST_MODE;
    usb_writel(DWC_REG_GUSBCFG, reg);
    proc_usleep(50000);

    if (dwc_core_soft_reset() != 0) {
        klog("dwc2: host init soft_reset_failed\n");
        return -1;
    }

    /* Re-assert GUSBCFG after soft reset: ModeSelect (UTMI) survives per spec,
       but force_host_mode does not. Re-apply to be safe. */
    reg = usb_readl(DWC_REG_GUSBCFG);
    reg &= ~(DWC_GUSBCFG_PHYIF | DWC_GUSBCFG_ULPI_UTMI_SEL |
            DWC_GUSBCFG_SRPCAP | DWC_GUSBCFG_HNPCAP |
            DWC_GUSBCFG_PHY_LP_CLK_SEL | DWC_GUSBCFG_ULPI_FSLS | DWC_GUSBCFG_ULPI_CLK_SUSP_M |
            DWC_GUSBCFG_ULPI_DRV_EXT_VBUS | DWC_GUSBCFG_TSDLINEPULSE |
            DWC_GUSBCFG_FORCE_DEV_MODE | DWC_GUSBCFG_USBTRDTIM_MASK);
    reg |= (DWC_USBTRDTIM_UTMI_8BIT << DWC_GUSBCFG_USBTRDTIM_SHIFT);
    reg |= DWC_GUSBCFG_FORCE_HOST_MODE;
    usb_writel(DWC_REG_GUSBCFG, reg);
    proc_usleep(25000);

    usb_writel(DWC_REG_GRXFSIZ, DWC_RX_FIFO_SIZE);
    usb_writel(DWC_REG_GNPTXFSIZ, (DWC_NP_TX_FIFO_SIZE << 16) | DWC_RX_FIFO_SIZE);
    usb_writel(DWC_REG_HPTXFSIZ, (DWC_P_TX_FIFO_SIZE << 16) |
            (DWC_RX_FIFO_SIZE + DWC_NP_TX_FIFO_SIZE));
    if (dwc_flush_fifos() != 0) {
        return -1;
    }

    usb_writel(DWC_REG_HCFG, DWC_HCFG_FSLSPCLKSEL_30_60MHZ);
    usb_writel(DWC_REG_HFIR, DWC_HFIR_60MHZ_FSLS);
    reg = usb_readl(DWC_REG_GOTGCTL);
    reg &= ~DWC_GOTGCTL_HSTSETHNPEN;
    usb_writel(DWC_REG_GOTGCTL, reg);
    if (dwc_host_halt_all_channels() != 0) {
        klog("dwc2: host init halt_all_channels_failed\n");
        return -1;
    }
    usb_writel(DWC_REG_HAINTMSK, 0);
    usb_writel(DWC_REG_GINTSTS, 0xFFFFFFFFu);
    usb_writel(DWC_REG_GINTMSK, 0);
    reg = usb_readl(DWC_REG_GAHBCFG);
    reg &= ~DWC_GAHBCFG_GLBL_INTR_EN;
    reg |= DWC_GAHBCFG_DMA_EN | DWC_GAHBCFG_WAIT_AXI_WRITES;
    usb_writel(DWC_REG_GAHBCFG, reg);
    dwc_port_write(DWC_HPRT_PWR, 0);
    proc_usleep(100000);
    dwc2_ack_port_change();
    return 0;
}

int dwc2_init(ewokos_addr_t mmio_base) {
    if (_ready) {
        return 0;
    }
    if (dma_pool_init() != 0) {
        return -1;
    }
    _usb_base = mmio_base + USB_CORE_OFFSET;
    if (dwc_host_init() != 0) {
        return -1;
    }
    _ready = true;
    return 0;
}

int dwc2_reinit(void) {
    if (_usb_base == 0) {
        return -1;
    }
    if (dwc_host_init() != 0) {
        _ready = false;
        return -1;
    }
    _ready = true;
    return 0;
}

bool dwc2_ready(void) {
    return _ready;
}

/* ---------------- channel transfers ---------------- */

static int dwc_channel_wait(int ch, uint32_t timeout_ms, uint32_t* hcint_out, uint32_t* actual_out) {
    uint32_t waited = 0;
    uint32_t hcint;
    while (waited < timeout_ms) {
        hcint = usb_readl(DWC_HCINT(ch));
        if ((hcint & (DWC_HCINT_XFRC | DWC_HCINT_CHH | DWC_HCINT_AHBERR |
                DWC_HCINT_STALL | DWC_HCINT_NAK | DWC_HCINT_TXERR | DWC_HCINT_BBLERR |
                DWC_HCINT_DTERR)) != 0) {
            if (hcint_out != NULL) {
                *hcint_out = hcint;
            }
            if (actual_out != NULL) {
                *actual_out = (uint32_t)(usb_readl(DWC_HCTSIZ(ch)) & DWC_HCTSIZ_XFERSIZE_MASK);
            }
            return 0;
        }
        proc_usleep(1000);
        waited++;
    }
    return -1;
}

/* returns bytes moved, DWC2_XFER_RETRY on NAK/NYET/frame-overrun,
   -1 on timeout or hard handshake error. _xfer_lock must be held. */
static int dwc_channel_transfer(int ch, uint8_t dev_addr, uint8_t ep_num, bool dir_in,
        bool low_speed, uint8_t ep_type, uint16_t max_packet, uint32_t pid,
        uint32_t buffer_phys, uint32_t length, uint32_t timeout_ms) {
    uint32_t hcchar;
    uint32_t hcchar_start;
    uint32_t hctsiz;
    uint32_t hcint = 0;
    uint32_t remaining = 0;
    uint32_t buffer_bus = 0;
    uint32_t packets = (length == 0) ? 1u : (uint32_t)((length + max_packet - 1u) / max_packet);

    if (buffer_phys != 0) {
        buffer_bus = dwc_dma_bus_addr(buffer_phys);
    }

    dwc_channel_prepare(ch);
    dwc_writel_sync(DWC_HCINT(ch), 0xFFFFFFFFu);
    dwc_writel_sync(DWC_HCINTMSK(ch), DWC_HCINT_XFRC | DWC_HCINT_CHH | DWC_HCINT_AHBERR |
            DWC_HCINT_STALL | DWC_HCINT_NAK | DWC_HCINT_ACK |
            DWC_HCINT_NYET | DWC_HCINT_TXERR | DWC_HCINT_BBLERR |
            DWC_HCINT_DTERR);
    (void)usb_readl(DWC_HCTSIZ(ch));
    (void)usb_readl(DWC_HCSPLT(ch));
    dwc_writel_sync(DWC_HCDMA(ch), buffer_bus);

    hctsiz = (length & DWC_HCTSIZ_XFERSIZE_MASK) |
            ((packets & 0x3FFu) << DWC_HCTSIZ_PKTCNT_SHIFT) |
            ((pid & 0x3u) << DWC_HCTSIZ_PID_SHIFT);
    dwc_writel_sync(DWC_HCTSIZ(ch), hctsiz);

    hcchar = (uint32_t)(max_packet & 0x7FFu) |
            ((uint32_t)(ep_num & 0x0Fu) << DWC_HCCHAR_EPNUM_SHIFT) |
            ((uint32_t)ep_type << DWC_HCCHAR_EPTYPE_SHIFT) |
            ((uint32_t)(dev_addr & 0x7Fu) << DWC_HCCHAR_DEVADDR_SHIFT);
    if (dir_in) {
        hcchar |= DWC_HCCHAR_EPDIR_IN;
    }
    if (low_speed) {
        hcchar |= DWC_HCCHAR_LSPDDEV;
    }
    /* Periodic transfers execute in the frame whose parity matches ODDFRM:
       schedule for the next frame relative to the current frame number. */
    if (ep_type == 1 || ep_type == 3) {
        uint32_t frnum = (usb_readl(DWC_REG_HFNUM) >> 0) & 0xFFFFu;
        if ((frnum & 1u) == 0) {
            hcchar |= DWC_HCCHAR_ODDFRM;
        }
    }
    dwc_writel_sync(DWC_HCCHAR(ch), hcchar);

    hcchar_start = usb_readl(DWC_HCCHAR(ch));
    hcchar_start &= ~DWC_HCCHAR_CHDIS;
    hcchar_start |= DWC_HCCHAR_CHENA;
    dwc_writel_sync(DWC_HCCHAR(ch), hcchar_start);

    if (dwc_channel_wait(ch, timeout_ms, &hcint, &remaining) != 0) {
        _last_hcint = usb_readl(DWC_HCINT(ch));
        (void)dwc_channel_halt(ch, 20);
        return -1;
    }

    usb_writel(DWC_HCINT(ch), hcint);
    _last_hcint = hcint;
    if (hcint & (DWC_HCINT_AHBERR | DWC_HCINT_STALL | DWC_HCINT_TXERR |
            DWC_HCINT_BBLERR | DWC_HCINT_DTERR)) {
        (void)dwc_channel_halt(ch, 20);
        return -1;
    }
    if (hcint & (DWC_HCINT_NAK | DWC_HCINT_NYET | DWC_HCINT_FRMOVRUN)) {
        (void)dwc_channel_halt(ch, 20);
        return DWC2_XFER_RETRY;
    }
    if ((hcint & DWC_HCINT_XFRC) == 0 && (hcint & DWC_HCINT_CHH) != 0 && length != 0) {
        (void)dwc_channel_halt(ch, 20);
        return -1;
    }
    return (int)(length - (usb_readl(DWC_HCTSIZ(ch)) & DWC_HCTSIZ_XFERSIZE_MASK));
}

static int dwc_control_stage_transfer(int ch, uint8_t addr,
        uint8_t ep_num, bool dir_in, bool low_speed, uint8_t ep_type,
        uint16_t max_packet, uint32_t pid, uint32_t buffer_phys,
        uint32_t length, uint32_t timeout_ms) {
    int attempt;
    int ret = -1;

    for (attempt = 1; attempt <= 3; attempt++) {
        ret = dwc_channel_transfer(ch, addr, ep_num, dir_in, low_speed, ep_type,
                max_packet, pid, buffer_phys, length, timeout_ms);
        if (ret >= 0) {
            return ret;
        }
        if (attempt < 3) {
            proc_usleep(5000);
        }
    }
    return ret;
}

/* _xfer_lock must be held by the caller */
static int usb_control_msg_unlocked(uint8_t addr, bool low_speed, uint8_t ep_mps,
        const usb_setup_pkt_t* setup, void* data, bool data_in) {
    uint32_t mark = dma_pool_mark();
    usb_setup_pkt_t* setup_dma;
    uint32_t setup_phys = 0;
    uint8_t* payload = NULL;
    uint32_t payload_phys = 0;
    uint8_t* status_zlp = NULL;
    uint32_t status_zlp_phys = 0;
    int ret;
    int actual = 0;

    setup_dma = (usb_setup_pkt_t*)dma_pool_alloc(sizeof(*setup_dma), 8, &setup_phys);
    if (setup_dma == NULL) {
        return -1;
    }
    memcpy(setup_dma, setup, sizeof(*setup_dma));

    if (setup->wLength > 0) {
        payload = (uint8_t*)dma_pool_alloc(setup->wLength, 8, &payload_phys);
        if (payload == NULL) {
            dma_pool_rewind(mark);
            return -1;
        }
        if (!data_in && data != NULL) {
            memcpy(payload, data, setup->wLength);
        }
    }

    status_zlp = (uint8_t*)dma_pool_alloc(8, 8, &status_zlp_phys);
    if (status_zlp == NULL) {
        dma_pool_rewind(mark);
        return -1;
    }
    (void)status_zlp;

    ret = dwc_control_stage_transfer(DWC_CH_CTRL, addr, 0, false, low_speed, 0,
            ep_mps, DWC_PID_SETUP, setup_phys, sizeof(*setup_dma),
            USB_CTRL_SETUP_TIMEOUT_MS);
    if (ret < 0) {
        dma_pool_rewind(mark);
        return -1;
    }

    if (setup->wLength > 0) {
        ret = dwc_control_stage_transfer(DWC_CH_CTRL, addr, 0, data_in, low_speed, 0,
                ep_mps, DWC_PID_DATA1, payload_phys, setup->wLength,
                USB_CTRL_DATA_TIMEOUT_MS);
        if (ret < 0) {
            dma_pool_rewind(mark);
            return -1;
        }
        actual = ret;
        if (data_in && data != NULL && ret > 0) {
            memcpy(data, payload, ret);
        }
    }

    ret = dwc_control_stage_transfer(DWC_CH_CTRL, addr, 0, !data_in, low_speed, 0,
            ep_mps, DWC_PID_DATA1, status_zlp_phys, 0,
            USB_CTRL_STATUS_TIMEOUT_MS);
    dma_pool_rewind(mark);
    if (ret < 0) {
        return -1;
    }
    return actual;
}

int dwc2_control_xfer(uint8_t addr, bool low_speed, uint8_t ep_mps,
        const usb_setup_pkt_t* setup, void* data, bool data_in) {
    int ret;

    if (!_ready) {
        return -1;
    }
    pthread_mutex_lock(&_xfer_lock);
    ret = usb_control_msg_unlocked(addr, low_speed, ep_mps, setup, data, data_in);
    pthread_mutex_unlock(&_xfer_lock);
    return ret;
}

/* ---------------- interrupt-IN (HID) ---------------- */

int dwc2_int_in_xfer(uint8_t addr, bool low_speed, uint8_t ep_num,
        uint16_t max_packet, uint8_t* toggle, void* data, uint16_t size) {
    uint8_t* payload;
    uint32_t payload_phys = 0;
    uint32_t mark;
    int ret;

    if (!_ready || toggle == NULL || data == NULL) {
        return -1;
    }
    pthread_mutex_lock(&_xfer_lock);
    mark = dma_pool_mark();

    payload = (uint8_t*)dma_pool_alloc(size, 8, &payload_phys);
    if (payload == NULL) {
        pthread_mutex_unlock(&_xfer_lock);
        return -1;
    }

    ret = dwc_channel_transfer(DWC_CH_INT_IN, addr, ep_num, true,
            low_speed, 3, max_packet, *toggle ? DWC_PID_DATA1 : DWC_PID_DATA0,
            payload_phys, size, USB_INT_IN_TIMEOUT_MS);
    if (ret >= 0) {
        if (ret > 0) {
            memcpy(data, payload, ret);
        }
        /* resync toggle from the core: HCTSIZ.PID holds the pid for the
           next transaction and advances on every accepted packet, ZLPs
           included (a blind xor skips ZLPs and desyncs -> DTERR storms) */
        uint32_t hwpid = (usb_readl(DWC_HCTSIZ(DWC_CH_INT_IN)) >> DWC_HCTSIZ_PID_SHIFT) & 0x3u;
        *toggle = (hwpid == DWC_PID_DATA1) ? 1u : 0u;
    }
    else if (ret == -1 && (_last_hcint & DWC_HCINT_STALL) != 0) {
        /* let the policy layer clear the device-side halt */
        dma_pool_rewind(mark);
        pthread_mutex_unlock(&_xfer_lock);
        return DWC2_XFER_RETRY;
    }
    else if (_last_hcint & DWC_HCINT_DTERR) {
        /* Blindly xor-ing the toggle after DTERR can lock us into a
           persistent DATA0/DATA1 ping-pong on flaky HID endpoints. Prefer
           the core's next-PID view when available, and only fall back to an
           xor when HCTSIZ does not expose a DATA0/DATA1 state. */
        uint32_t hwpid = (usb_readl(DWC_HCTSIZ(DWC_CH_INT_IN)) >> DWC_HCTSIZ_PID_SHIFT) & 0x3u;
        if (hwpid == DWC_PID_DATA0 || hwpid == DWC_PID_DATA1) {
            *toggle = (hwpid == DWC_PID_DATA1) ? 1u : 0u;
        }
        else {
            *toggle ^= 1u;
        }
        dma_pool_rewind(mark);
        pthread_mutex_unlock(&_xfer_lock);
        return 0;
    }
    else if (ret == DWC2_XFER_RETRY) {
        /* NAK: idle device, nothing to do */
        dma_pool_rewind(mark);
        pthread_mutex_unlock(&_xfer_lock);
        return 0;
    }
    dma_pool_rewind(mark);
    pthread_mutex_unlock(&_xfer_lock);
    return ret;
}

/* ---------------- bulk (mass storage) ---------------- */

/* one bulk transaction on a dedicated MSC channel; NAK returns
   DWC2_XFER_RETRY so the caller can re-arm without disturbing the toggle */
int dwc2_bulk_xfer(bool dir_in, uint8_t addr, bool low_speed, uint8_t ep_num,
        uint16_t max_packet, uint8_t* toggle, void* data, uint32_t length,
        uint32_t timeout_ms) {
    int ch = dir_in ? DWC_CH_BULK_IN : DWC_CH_BULK_OUT;
    uint32_t mark;
    uint8_t* payload;
    uint32_t payload_phys = 0;
    uint32_t pid;
    int ret;

    if (!_ready || toggle == NULL || data == NULL) {
        return -1;
    }
    pthread_mutex_lock(&_xfer_lock);
    mark = dma_pool_mark();

    payload = (uint8_t*)dma_pool_alloc(length == 0 ? 1 : length, 8, &payload_phys);
    if (payload == NULL) {
        pthread_mutex_unlock(&_xfer_lock);
        return -1;
    }
    if (!dir_in) {
        memcpy(payload, data, length);
    }

    pid = (*toggle != 0) ? DWC_PID_DATA1 : DWC_PID_DATA0;
    ret = dwc_channel_transfer(ch, addr, ep_num, dir_in, low_speed,
            2, max_packet, pid, payload_phys, length, timeout_ms);

    if (ret == DWC2_XFER_RETRY) {
        dma_pool_rewind(mark);
        pthread_mutex_unlock(&_xfer_lock);
        return DWC2_XFER_RETRY;
    }
    if (ret >= 0 && dir_in && ret > 0) {
        memcpy(data, payload, ret);
    }
    /* resync the toggle from the core's next-PID view, same as the
       interrupt-IN path: blind xor desyncs across ZLPs */
    uint32_t hwpid = (usb_readl(DWC_HCTSIZ(ch)) >> DWC_HCTSIZ_PID_SHIFT) & 0x3u;
    if (hwpid == DWC_PID_DATA0 || hwpid == DWC_PID_DATA1) {
        *toggle = (hwpid == DWC_PID_DATA1) ? 1u : 0u;
    }
    else if (ret >= 0) {
        *toggle ^= 1u;
    }
    dma_pool_rewind(mark);
    pthread_mutex_unlock(&_xfer_lock);
    return ret;
}

/* bulk-only reset + clear endpoint halts */
void dwc2_msc_recover(uint8_t addr, bool low_speed, uint8_t ctrl_mps,
        uint8_t iface_num, uint8_t ep_in, uint8_t ep_out) {
    usb_setup_pkt_t setup;

    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = USB_REQTYPE_CLASS_IFACE_OUT;
    setup.bRequest = USB_MSC_REQ_RESET;
    setup.wIndex = iface_num;
    (void)dwc2_control_xfer(addr, low_speed, ctrl_mps, &setup, NULL, false);
    proc_usleep(10000);

    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = USB_REQTYPE_STD_OUT;
    setup.bRequest = USB_REQ_CLEAR_FEATURE;
    setup.wValue = USB_FEAT_ENDPOINT_HALT;
    setup.wIndex = ep_in;
    (void)dwc2_control_xfer(addr, low_speed, ctrl_mps, &setup, NULL, false);
    setup.wIndex = ep_out;
    (void)dwc2_control_xfer(addr, low_speed, ctrl_mps, &setup, NULL, false);
}
