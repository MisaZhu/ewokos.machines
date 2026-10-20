/* SC8280XP USB host glue for the Dragon Q8B.
 *
 * Qualcomm firmware powers and calibrates the multi-port DWC3 core before
 * ExitBootServices.  The controller exposes a standard xHCI register file at
 * 0x0a400000; this layer takes it over, then reuses the shared EwokOS USB
 * enumeration, HID, hub and mass-storage policy.
 */
#include <bsp/bsp_usb.h>
#include <bsp/xhci.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <ewoksys/vfs.h>
#include <ewoksys/proc.h>
#include <ewoksys/syscall.h>
#include <ewoksys/mmio.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/klog.h>
#include <ewoksys/usbmsc.h>

#define BSP_USB_MAX_DEVS 16
#define BSP_USB_NUM_XHC 1
#define BSP_USB_MAX_PORTS (BSP_USB_NUM_XHC * XHCI_MAX_PORTS)

/* sc8280xp.dtsi usb_2_dwc3, used by the board's conventional USB ports. */
#define Q8B_XHCI_PHY_BASE 0x0a400000ULL
#define XHCI_BAR_VA_BASE  0x50000000ULL
#define XHCI_BAR_MAP_SIZE 0x10000u

/* The copied policy layer still contains dead UHCI branches. Keeping tiny
 * local stubs makes those branches explicit while the compiler eliminates
 * them; every actual Q8B device is xHCI-owned. */
#define UHCI_MAX_CONTROLLERS 0
#define UHCI_PORTS_PER_CTRL 0
static int uhci_init(void) { return -1; }
static int uhci_reinit(void) { return -1; }
static int uhci_port_count(void) { return 0; }
static bool uhci_port_connected(int port) { (void)port; return false; }
static int uhci_reset_port(int port) { (void)port; return -1; }
static void uhci_ack_port_change(int port) { (void)port; }
static void uhci_recover_port(int port) { (void)port; }
static int uhci_control_xfer(int port, bool low_speed, uint8_t addr,
        uint8_t mps, const usb_setup_pkt_t* setup, void* data, bool dir_in) {
    (void)port; (void)low_speed; (void)addr; (void)mps;
    (void)setup; (void)data; (void)dir_in;
    return -1;
}
static int uhci_int_in_xfer(int port, bool low_speed, uint8_t addr,
        uint8_t ep, uint16_t mps, uint8_t* toggle, void* data,
        uint16_t size) {
    (void)port; (void)low_speed; (void)addr; (void)ep; (void)mps;
    (void)toggle; (void)data; (void)size;
    return -1;
}
static int uhci_bulk_xfer(int port, bool low_speed, bool dir_in,
        uint8_t addr, uint8_t ep, uint16_t mps, uint8_t* toggle,
        void* data, uint32_t len, uint32_t timeout_ms) {
    (void)port; (void)low_speed; (void)dir_in; (void)addr; (void)ep;
    (void)mps; (void)toggle; (void)data; (void)len; (void)timeout_ms;
    return -1;
}

/* interrupt-IN pacing: the shared layer passes bInterval through; a
   polled controller honours it as a floor and stretches it while the
   endpoint keeps NAKing.

   The floor/cap directly bound keyboard event loss: a HID keyboard runs
   with Set_Idle(0) (report on change only), so a key pressed AND released
   inside one poll gap leaves the endpoint back at its baseline state and
   the press is never seen. The old 8ms floor ignored fast keyboards whose
   real bInterval is 1-2ms, and the 40ms cap let an idle endpoint stretch
   far past the device's own report rate, so quick taps after a pause were
   swallowed whole. Honour the device down to 2ms and cap the idle stretch
   near a normal report period; the tighter cadence stays affordable
   because the NAK poll now returns fast (UHCI_INT_IN_TIMEOUT_MS). */
#define BSP_USB_INT_MIN_INTERVAL_MS 2u
#define BSP_USB_INT_MAX_INTERVAL_MS 16u
#define BSP_USB_INT_IDLE_STRETCH_2 32u
#define BSP_USB_INT_IDLE_STRETCH_4 96u

struct bsp_usb_dev {
    bool used;
    /* true: owned by an xHCI controller, the xhci_dev_t below carries the
       slot/ring state and the uhci_* fields are unused */
    bool on_xhci;
    xhci_dev_t xdev;
    uint8_t addr;
    bool low_speed;
    uint8_t ctrl_mps;
    /* uhci flat (0-based) port this device hangs off; transfers of a
       device behind a hub still run on its root port's controller */
    int root_flat;
    /* single polled interrupt-IN endpoint (the HID report endpoint);
       uhci only, xHCI arms its rings in hardware */
    uint8_t int_ep_addr;
    uint16_t int_mps;
    uint8_t int_toggle;
    uint32_t int_interval_ms;
    uint64_t int_next_ms;
    uint32_t int_idle_polls;
};

static bsp_usb_dev_t _devs[BSP_USB_MAX_DEVS];
static bool _prev_connected[BSP_USB_MAX_PORTS];
static bool _inited = false;

/* xHCI state: present controllers + per-controller activity (see
   bsp_usb_poll: a controller with no attached device and no pending
   teardown is skipped so an idle port does not burn event-ring polls) */
static xhci_hc_t _xhcs[BSP_USB_NUM_XHC];
static uint32_t _xhc_dev_count[BSP_USB_NUM_XHC];
static bool _xhc_dirty[BSP_USB_NUM_XHC];

typedef struct {
    bool ready;
    bool claimed;
    bsp_usb_dev_t* dev;
    uint8_t iface_num;
    uint8_t ep_in;   /* endpoint address (with USB_ENDPOINT_IN) */
    uint8_t ep_out;
    uint16_t mps_in;
    uint16_t mps_out;
    uint8_t toggle_in;
    uint8_t toggle_out;
    uint32_t tag;
    uint32_t sector_count;
    uint32_t sector_size;
    int child_pid;
} usb_msc_t;

static usb_msc_t _msc;
/* set by msc_attach, serviced (fork) from bsp_usb_poll: the spawn must
   wait until /dev/hid0 exists, but the first port scan that claims the
   MSC device runs inside usbhostd main() BEFORE device_run() registers
   it — spawning there races the daemon registration and usbfat32fsd
   dies with "usb dev /dev/hid0 not found" whenever it wins the CPU */
static bool _msc_mount_pending = false;
static void msc_mount_spawn(void);

static inline uint32_t be32(const void* p) {
    const uint8_t* b = (const uint8_t*)p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
            ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

static inline void put_be32(void* p, uint32_t v) {
    uint8_t* b = (uint8_t*)p;
    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);
    b[3] = (uint8_t)v;
}

/* ---- xHCI glue: fixed SC8280XP controller, flat ports, speed codes ---- */

/* flat 1-based port -> xHCI controller + 1-based hc port; the xHCI range
   starts after all UHCI ports so existing UHCI port indices never move */
static xhci_hc_t* flat_to_xhc(int port, int* hc_port) {
    int rel = port - uhci_port_count();
    for (int i = 0; i < BSP_USB_NUM_XHC; ++i) {
        if (!_xhcs[i].present) {
            continue;
        }
        if (rel >= 1 && rel <= (int)_xhcs[i].num_ports) {
            if (hc_port != NULL) {
                *hc_port = rel;
            }
            return &_xhcs[i];
        }
        rel -= (int)_xhcs[i].num_ports;
    }
    return NULL;
}

static int xhci_hc_index(xhci_hc_t* hc) {
    for (int i = 0; i < BSP_USB_NUM_XHC; ++i) {
        if (&_xhcs[i] == hc) {
            return i;
        }
    }
    return -1;
}

static int xhci_port_count(void) {
    int count = 0;
    for (int i = 0; i < BSP_USB_NUM_XHC; ++i) {
        if (_xhcs[i].present) {
            count += (int)_xhcs[i].num_ports;
        }
    }
    return count;
}

static int speed_to_xhci(int speed) {
    switch (speed) {
    case BSP_USB_SPEED_LOW:
        return XHCI_SPEED_LOW;
    case BSP_USB_SPEED_HIGH:
        return XHCI_SPEED_HIGH;
    case BSP_USB_SPEED_SUPER:
        return XHCI_SPEED_SUPER;
    case BSP_USB_SPEED_SUPER_PLUS:
        return XHCI_SPEED_SUPER_PLUS;
    default:
        return XHCI_SPEED_FULL;
    }
}

static int speed_from_xhci(int speed) {
    switch (speed) {
    case XHCI_SPEED_LOW:
        return BSP_USB_SPEED_LOW;
    case XHCI_SPEED_HIGH:
        return BSP_USB_SPEED_HIGH;
    case XHCI_SPEED_SUPER:
        /* SS must round-trip as SS: mapping it to HIGH made the attach
           program the slot context with the wrong speed, real xHCI rejects
           Address Device with Parameter Error (QEMU accepts it), the
           enumeration failed 6 times and the escalation HCRST the whole
           controller - killing the mouse sharing it. Root cause of the
           "insert USB3 stick, mouse stutters then dies" bug. */
        return BSP_USB_SPEED_SUPER;
    case XHCI_SPEED_SUPER_PLUS:
        return BSP_USB_SPEED_SUPER_PLUS;
    default:
        return BSP_USB_SPEED_FULL;
    }
}

/*
 * Never fail hard: ipcserv blocks in ipc_wait_ready() until the daemon
 * registers its mount point, so degrading to "no xhci" is the only safe
 * path; every entry point below is gated on hc->present.
 *
 * UEFI has already enabled the GCC votes, power domain, HS PHYs and QMP PHYs.
 * Map the live xHCI aperture and take ownership with a normal HCRST.
 */
static int xhci_bring_up(void) {
    uint32_t cap;

    if (syscall3(SYS_MEM_MAP, XHCI_BAR_VA_BASE, Q8B_XHCI_PHY_BASE,
            XHCI_BAR_MAP_SIZE) != XHCI_BAR_VA_BASE) {
        klog("bsp_usb: q8b xhci map failed phy=%llx\n",
                (unsigned long long)Q8B_XHCI_PHY_BASE);
        return 0;
    }
    cap = get32(XHCI_BAR_VA_BASE);
    klog("bsp_usb: q8b xhci cap=%08x phy=%llx\n", cap,
            (unsigned long long)Q8B_XHCI_PHY_BASE);
    if ((cap >> 16) < 0x0090u || (cap >> 16) == 0xffffu)
        return 0;
    if (xhci_dma_init() != 0) {
        klog("bsp_usb: xhci dma init failed\n");
        return 0;
    }
    return xhci_init(&_xhcs[0], 0, XHCI_BAR_VA_BASE) == 0 ? 1 : 0;
}

/* ---- init / poll ---- */

int bsp_usb_init(void) {
    if (_inited) {
        return 0;
    }
    _inited = true;
    memset(_devs, 0, sizeof(_devs));
    memset(_prev_connected, 0, sizeof(_prev_connected));
    memset(_xhc_dev_count, 0, sizeof(_xhc_dev_count));
    memset(_xhc_dirty, 0, sizeof(_xhc_dirty));
    memset(_xhcs, 0, sizeof(_xhcs));

    xhci_bring_up(); /* degrades to zero controllers, never fails */
    if (uhci_port_count() == 0 && xhci_port_count() == 0) {
        klog("bsp_usb: no usb controller found, running without usb\n");
    }
    return 0;
}

int bsp_usb_reinit(void) {
    int found = 0;

    (void)uhci_reinit();
    /* a re-init drops every device: the controllers forget all addresses
       and slots, so the policy layer must re-enumerate the tree from
       scratch */
    if (_msc.child_pid > 0)
        dev_cntl_by_pid(_msc.child_pid, USBFS_CMD_QUIT, NULL, NULL);
    memset(_devs, 0, sizeof(_devs));
    memset(_prev_connected, 0, sizeof(_prev_connected));
    memset(_xhc_dev_count, 0, sizeof(_xhc_dev_count));
    memset(&_msc, 0, sizeof(_msc));
    _msc_mount_pending = false;
    for (int i = 0; i < BSP_USB_NUM_XHC; ++i) {
        memset(&_xhcs[i], 0, sizeof(_xhcs[i]));
        if (xhci_init(&_xhcs[i], i, XHCI_BAR_VA_BASE +
                (uint32_t)i * XHCI_BAR_MAP_SIZE) == 0) {
            _xhc_dirty[i] = true;
            found++;
        }
        else {
            klog("bsp_usb: xhci%d reinit failed\n", i);
        }
    }
    return (found > 0 || uhci_port_count() > 0) ? 0 : -1;
}

void bsp_usb_poll(void) {
    if (_msc_mount_pending) {
        _msc_mount_pending = false;
        msc_mount_spawn();
    }
    for (int i = 0; i < BSP_USB_NUM_XHC; ++i) {
        if (_xhcs[i].present && (_xhc_dev_count[i] > 0 || _xhc_dirty[i])) {
            _xhc_dirty[i] = false;
            xhci_process_events(&_xhcs[i]);
        }
    }
}

/* ---- root ports (flat 1-based across all controllers) ---- */

int bsp_usb_root_port_count(void) {
    return uhci_port_count() + xhci_port_count();
}

bool bsp_usb_root_port_connected(int port) {
    int hc_port;
    xhci_hc_t* hc;

    if (port >= 1 && port <= uhci_port_count()) {
        return uhci_port_connected(port - 1);
    }
    hc = flat_to_xhc(port, &hc_port);
    if (hc == NULL) {
        return false;
    }
    return xhci_port_connected(hc, hc_port);
}

uint32_t bsp_usb_root_port_changes(void) {
    uint32_t changes = 0;
    int count = uhci_port_count();

    for (int i = 0; i < count && i < BSP_USB_MAX_PORTS; ++i) {
        bool conn = uhci_port_connected(i);
        if (conn != _prev_connected[i]) {
            changes |= 1u << i;
            _prev_connected[i] = conn;
        }
        uhci_ack_port_change(i);
    }
    /* xHCI hot-plug: the latch lives inside the controller (PORTSC.CSC +
       port-change events), no host-side comparison needed */
    {
        int shift = count;
        for (int i = 0; i < BSP_USB_NUM_XHC; ++i) {
            if (!_xhcs[i].present) {
                continue;
            }
            changes |= xhci_port_take_changes(&_xhcs[i]) << shift;
            shift += (int)_xhcs[i].num_ports;
        }
    }
    return changes;
}

int bsp_usb_root_port_reset(int port) {
    int hc_port;
    xhci_hc_t* hc;
    int speed;

    if (port >= 1 && port <= uhci_port_count()) {
        speed = uhci_reset_port(port - 1);
        if (speed < 0) {
            return -1;
        }
        return speed == 0 ? BSP_USB_SPEED_LOW : BSP_USB_SPEED_FULL;
    }
    hc = flat_to_xhc(port, &hc_port);
    if (hc == NULL) {
        return -1;
    }
    speed = xhci_port_reset(hc, hc_port);
    if (speed < 0) {
        return -1;
    }
    return speed_from_xhci(speed);
}

/* ---- device lifecycle ---- */

/* Allocate the lowest USB device address (1..127) not currently held by an
   attached device; address 0 is the default/broadcast address and is never
   assigned. Scanning the live device table keeps addresses globally unique
   across all root ports. A plain incrementing counter that is reset on every
   port reset would hand the SAME address to devices enumerated on different
   ports (they share one UHCI bus), making the second device's control
   transfers answer with the first device's descriptors. */
static uint8_t usb_alloc_address(void) {
    for (int a = 1; a <= 127; ++a) {
        bool taken = false;
        for (int i = 0; i < BSP_USB_MAX_DEVS; ++i) {
            if (_devs[i].used && _devs[i].addr == (uint8_t)a) {
                taken = true;
                break;
            }
        }
        if (!taken) {
            return (uint8_t)a;
        }
    }
    return 0;
}

bsp_usb_dev_t* bsp_usb_device_attach(int root_port, int speed,
        bsp_usb_dev_t* parent_hub, int hub_port) {
    bsp_usb_dev_t* dev = NULL;
    usb_setup_pkt_t setup;
    uint8_t addr;
    int flat;
    bool on_xhci = false;
    xhci_hc_t* hc = NULL;
    int hc_port = 0;

    /*
     * Routing: a device behind a hub belongs to whichever controller owns
     * the hub, a root device to the controller that owns its port.
     */
    if (parent_hub != NULL) {
        on_xhci = parent_hub->on_xhci;
        if (on_xhci) {
            hc = parent_hub->xdev.hc;
            hc_port = parent_hub->xdev.root_port;
        }
        else {
            flat = parent_hub->root_flat;
        }
    }
    else if (root_port >= 1 && root_port <= uhci_port_count()) {
        flat = root_port - 1;
    }
    else {
        hc = flat_to_xhc(root_port, &hc_port);
        if (hc == NULL) {
            return NULL;
        }
        on_xhci = true;
    }

    for (int i = 0; i < BSP_USB_MAX_DEVS; ++i) {
        if (!_devs[i].used) {
            dev = &_devs[i];
            break;
        }
    }
    if (dev == NULL) {
        return NULL;
    }

    if (on_xhci) {
        memset(dev, 0, sizeof(*dev));
        if (xhci_device_attach(hc, hc_port, speed_to_xhci(speed),
                parent_hub != NULL ? &parent_hub->xdev : NULL, hub_port,
                &dev->xdev) != 0) {
            klog("bsp_usb: xhci attach failed port=%d speed=%d\n",
                    root_port, speed);
            return NULL;
        }
        dev->used = true;
        dev->on_xhci = true;
        {
            int idx = xhci_hc_index(dev->xdev.hc);
            if (idx >= 0) {
                _xhc_dev_count[idx]++;
            }
        }
        return dev;
    }

    addr = usb_alloc_address();
    if (addr == 0) {
        return NULL;
    }
    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = USB_REQTYPE_STD_OUT;
    setup.bRequest = USB_REQ_SET_ADDRESS;
    setup.wValue = addr;
    if (uhci_control_xfer(flat, speed == BSP_USB_SPEED_LOW, 0, 8,
            &setup, NULL, false) < 0) {
        klog("bsp_usb: uhci set_address failed flat=%d speed=%d\n", flat, speed);
        return NULL;
    }
    usleep(10000); /* USB spec: new address is valid after 2ms */

    memset(dev, 0, sizeof(*dev));
    dev->used = true;
    dev->addr = addr;
    dev->low_speed = (speed == BSP_USB_SPEED_LOW);
    dev->ctrl_mps = 8;
    dev->root_flat = flat;
    return dev;
}

void bsp_usb_device_detach(bsp_usb_dev_t* dev) {
    if (dev == NULL || !dev->used) {
        return;
    }
    if (dev->on_xhci) {
        int idx = xhci_hc_index(dev->xdev.hc);
        xhci_device_detach(&dev->xdev);
        if (idx >= 0 && _xhc_dev_count[idx] > 0) {
            _xhc_dev_count[idx]--;
            /* the teardown commands post events; drain them once even if
               this was the controller's last device */
            _xhc_dirty[idx] = true;
        }
    }
    memset(dev, 0, sizeof(*dev));
}

int bsp_usb_device_update_mps0(bsp_usb_dev_t* dev, uint8_t mps0) {
    if (dev == NULL || !dev->used) {
        return -1;
    }
    if (dev->on_xhci) {
        return xhci_update_mps0(&dev->xdev, mps0);
    }
    dev->ctrl_mps = mps0;
    return 0;
}

/* UHCI: no TT bookkeeping — devices behind a hub are addressed directly.
   xHCI: the slot must be marked as a hub before children can be
   addressed behind it (TT fields for LS/FS devices). */
int bsp_usb_device_configure_hub(bsp_usb_dev_t* dev, int num_ports) {
    if (dev == NULL || !dev->used) {
        return -1;
    }
    if (dev->on_xhci) {
        return xhci_configure_hub(&dev->xdev, num_ports);
    }
    (void)num_ports;
    return 0;
}

/* ---- transfers ---- */

int bsp_usb_control_xfer(bsp_usb_dev_t* dev, const usb_setup_pkt_t* setup,
        void* data, bool dir_in) {
    if (dev == NULL || !dev->used) {
        return -1;
    }
    if (dev->on_xhci) {
        return xhci_control_xfer(&dev->xdev, setup, data, dir_in);
    }
    return uhci_control_xfer(dev->root_flat, dev->low_speed, dev->addr,
            dev->ctrl_mps, setup, data, dir_in);
}

int bsp_usb_int_in_open(bsp_usb_dev_t* dev, uint8_t ep_addr, uint16_t mps,
        uint8_t interval) {
    uint32_t iv;

    if (dev == NULL || !dev->used) {
        return -1;
    }
    if (dev->on_xhci) {
        /* hardware-paced: the controller schedules the periodic TDs and
           only completes one when the device actually answers */
        return xhci_int_in_open(&dev->xdev, ep_addr, mps, interval);
    }
    iv = interval == 0 ? 10u : interval;
    if (iv < BSP_USB_INT_MIN_INTERVAL_MS) {
        iv = BSP_USB_INT_MIN_INTERVAL_MS;
    }
    if (iv > BSP_USB_INT_MAX_INTERVAL_MS) {
        iv = BSP_USB_INT_MAX_INTERVAL_MS;
    }
    dev->int_ep_addr = ep_addr;
    dev->int_mps = mps;
    dev->int_toggle = 0;
    dev->int_interval_ms = iv;
    dev->int_next_ms = 0;
    dev->int_idle_polls = 0;
    return 0;
}

/* effective cadence: base interval while data flows, stretched once the
   endpoint keeps NAKing, snapped back by the first report with data */
static uint32_t int_in_interval(const bsp_usb_dev_t* dev) {
    uint32_t iv = dev->int_interval_ms;
    if (dev->int_idle_polls >= BSP_USB_INT_IDLE_STRETCH_4) {
        iv *= 4u;
    }
    else if (dev->int_idle_polls >= BSP_USB_INT_IDLE_STRETCH_2) {
        iv *= 2u;
    }
    if (iv > BSP_USB_INT_MAX_INTERVAL_MS) {
        iv = BSP_USB_INT_MAX_INTERVAL_MS;
    }
    return iv;
}

int bsp_usb_int_in_poll(bsp_usb_dev_t* dev, uint8_t ep_addr, void* buf,
        int size) {
    uint64_t now;
    int ret;

    if (dev == NULL || !dev->used) {
        return -1;
    }
    if (dev->on_xhci) {
        return xhci_int_in_poll(&dev->xdev, ep_addr, buf, size);
    }
    if (dev->int_ep_addr != ep_addr || dev->int_mps == 0) {
        return 0;
    }
    now = kernel_tic_ms(0);
    if (now < dev->int_next_ms) {
        return 0;
    }
    dev->int_next_ms = now + int_in_interval(dev);

    ret = uhci_int_in_xfer(dev->root_flat, dev->low_speed, dev->addr,
            ep_addr & 0x0Fu, dev->int_mps, &dev->int_toggle, buf,
            (uint16_t)size);
    if (ret == -2) {
        return -2; /* endpoint STALL: let the policy layer clear halt */
    }
    if (ret > 0) {
        /* real data arrived: restore the base cadence immediately */
        dev->int_idle_polls = 0;
        dev->int_next_ms = now + dev->int_interval_ms;
        return ret;
    }
    if (ret == 0) {
        dev->int_idle_polls++;
        return 0;
    }
    /* hard error: UHCI tends to drop port-enable after babble/timeout,
       re-enable so the next poll does not die on a disabled port */
    uhci_recover_port(dev->root_flat);
    dev->int_idle_polls++;
    return -1;
}

int bsp_usb_bulk_open(bsp_usb_dev_t* dev, uint8_t ep_addr, uint16_t mps) {
    if (dev == NULL || !dev->used) {
        return -1;
    }
    if (dev->on_xhci) {
        /* real endpoint state: ring + doorbell, armed lazily on poll */
        return xhci_bulk_open(&dev->xdev, ep_addr, mps);
    }
    (void)ep_addr;
    (void)mps;
    /* stateless: endpoint state (toggle) is tracked per consumer */
    return 0;
}

int bsp_usb_bulk_xfer(bsp_usb_dev_t* dev, uint8_t ep_addr, void* data,
        int len, bool dir_in) {
    uint8_t toggle = 0;
    if (dev == NULL || !dev->used || len <= 0) {
        return -1;
    }
    if (dev->on_xhci) {
        return xhci_bulk_xfer(&dev->xdev, ep_addr, data,
                (uint32_t)len, dir_in, 2000u);
    }
    /* stateless one-shot: the MSC path keeps its own toggle bookkeeping
       and calls uhci_bulk_xfer() directly */
    return uhci_bulk_xfer(dev->root_flat, dev->low_speed, dir_in, dev->addr,
            ep_addr & 0x0Fu, 64, &toggle, data, (uint32_t)len, 2000u);
}

/* polled bulk IN (BT ACL/events): same armed-TD model as interrupt-IN.
   UHCI has no polled bulk consumer. */
int bsp_usb_bulk_in_poll(bsp_usb_dev_t* dev, uint8_t ep_addr, void* buf,
        int size) {
    if (dev == NULL || !dev->used) {
        return -1;
    }
    if (dev->on_xhci) {
        return xhci_int_in_poll(&dev->xdev, ep_addr, buf, size);
    }
    return -1;
}

/* bsp_usbbt.c claims xHCI-attached devices only (UHCI has no bulk
   endpoint model); exported because struct bsp_usb_dev is local here */
bool bsp_usb_xhci_owned(bsp_usb_dev_t* dev) {
    return dev != NULL && dev->used && dev->on_xhci;
}

int bsp_usb_ep_clear_halt(bsp_usb_dev_t* dev, uint8_t ep_addr) {
    usb_setup_pkt_t setup;
    int ret;

    if (dev == NULL || !dev->used) {
        return -1;
    }
    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = USB_REQTYPE_STD_EP_OUT;
    setup.bRequest = USB_REQ_CLEAR_FEATURE;
    setup.wValue = USB_FEAT_ENDPOINT_HALT;
    setup.wIndex = ep_addr;
    ret = bsp_usb_control_xfer(dev, &setup, NULL, false);
    if (dev->int_ep_addr == ep_addr) {
        dev->int_toggle = 0;
    }
    return ret;
}

/* ---------------- mass storage (bulk-only transport) ---------------- */

#define MSC_BULK_TIMEOUT_MS 800u  /* 单次数据阶段上限: 坏盘一次读最多吃 usbhostd 0.8s (HID 间隙有界) */
#define MSC_CBW_TIMEOUT_MS 500u

/* One bulk transaction. xHCI owns data toggles in its endpoint context. */
static int msc_bulk_xfer(bool dir_in, uint8_t ep_addr, uint16_t mps,
        uint8_t* toggle, void* data, uint32_t len, uint32_t timeout_ms) {
    if (_msc.dev->on_xhci) {
        (void)mps;
        (void)toggle;
        return xhci_bulk_xfer(&_msc.dev->xdev, ep_addr, data, len,
                dir_in, timeout_ms);
    }
    return uhci_bulk_xfer(_msc.dev->root_flat, _msc.dev->low_speed, dir_in,
            _msc.dev->addr, ep_addr & 0x0Fu, mps, toggle, data, len,
            timeout_ms);
}

/* bulk-only mass storage reset + endpoint unhalt, then resync toggles */
static void msc_recover(void) {
    usb_setup_pkt_t setup;

    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = USB_REQTYPE_CLASS_IFACE_OUT;
    setup.bRequest = 0xFF; /* bulk-only mass storage reset */
    setup.wIndex = _msc.iface_num;
    (void)bsp_usb_control_xfer(_msc.dev, &setup, NULL, false);

    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = USB_REQTYPE_STD_EP_OUT;
    setup.bRequest = USB_REQ_CLEAR_FEATURE;
    setup.wValue = USB_FEAT_ENDPOINT_HALT;
    setup.wIndex = _msc.ep_in;
    (void)bsp_usb_control_xfer(_msc.dev, &setup, NULL, false);
    setup.wIndex = _msc.ep_out;
    (void)bsp_usb_control_xfer(_msc.dev, &setup, NULL, false);

    _msc.toggle_in = 0;
    _msc.toggle_out = 0;
    if (!_msc.dev->on_xhci)
        uhci_recover_port(_msc.dev->root_flat);
}

/* CBW -> optional data phase -> CSW. dir_in: data phase is IN.
   Returns 0 on CSW status "passed", -1 otherwise. */
static int usb_msc_command(const uint8_t* cdb, uint8_t cdb_len, bool dir_in,
        void* data, uint32_t data_len) {
    usb_cbw_t cbw;
    usb_csw_t csw;
    uint8_t* payload = NULL;
    uint32_t tag;
    int ret = -1;

    if (!_msc.claimed || _msc.dev == NULL || cdb_len > 16) {
        return -1;
    }
    if (data_len > 0) {
        payload = (uint8_t*)malloc(data_len);
        if (payload == NULL) {
            return -1;
        }
        if (!dir_in) {
            memcpy(payload, data, data_len);
        }
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        tag = ++_msc.tag;
        if (tag == 0) {
            tag = ++_msc.tag;
        }
        memset(&cbw, 0, sizeof(cbw));
        cbw.dCBWSignature = USB_MSC_CBW_SIG;
        cbw.dCBWTag = tag;
        cbw.dCBWDataTransferLength = data_len;
        cbw.bmCBWFlags = dir_in ? 0x80u : 0x00u;
        cbw.bCBWLUN = 0;
        cbw.bCBWCBLength = cdb_len;
        memcpy(cbw.CBWCB, cdb, cdb_len);

        if (msc_bulk_xfer(false, _msc.ep_out, _msc.mps_out, &_msc.toggle_out,
                &cbw, sizeof(cbw), MSC_CBW_TIMEOUT_MS) != (int)sizeof(cbw)) {
            msc_recover();
            continue;
        }

        if (data_len > 0) {
            int xret = msc_bulk_xfer(dir_in,
                    dir_in ? _msc.ep_in : _msc.ep_out,
                    dir_in ? _msc.mps_in : _msc.mps_out,
                    dir_in ? &_msc.toggle_in : &_msc.toggle_out,
                    payload, data_len, MSC_BULK_TIMEOUT_MS);
            if (xret != (int)data_len) {
                msc_recover();
                continue;
            }
        }

        if (msc_bulk_xfer(true, _msc.ep_in, _msc.mps_in, &_msc.toggle_in,
                &csw, sizeof(csw), MSC_BULK_TIMEOUT_MS) != (int)sizeof(csw)) {
            msc_recover();
            continue;
        }

        if (csw.dCSWSignature != USB_MSC_CSW_SIG || csw.dCSWTag != tag) {
            msc_recover();
            continue;
        }
        if (csw.bCSWStatus != 0) {
            ret = -1;
            goto out;
        }
        if (dir_in && data != NULL && data_len > 0) {
            memcpy(data, payload, data_len);
        }
        ret = 0;
        goto out;
    }

out:
    free(payload);
    return ret;
}

static int usb_msc_test_unit_ready(void) {
    uint8_t cdb[6];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_OPCODE_TEST_UNIT_READY;
    return usb_msc_command(cdb, 6, false, NULL, 0);
}

static int usb_msc_sync_cache(void) {
    uint8_t cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_OPCODE_SYNC_CACHE10;
    return usb_msc_command(cdb, 10, false, NULL, 0);
}

static int msc_attach(bsp_usb_dev_t* dev, uint8_t iface_num,
        uint8_t ep_in, uint8_t ep_out, uint16_t mps_in, uint16_t mps_out) {
    uint8_t inquiry[36];
    uint8_t capacity[8];
    uint8_t cdb[10];

    memset(&_msc, 0, sizeof(_msc));
    _msc.claimed = true;
    _msc.dev = dev;
    _msc.iface_num = iface_num;
    _msc.ep_in = ep_in;
    _msc.ep_out = ep_out;
    _msc.mps_in = mps_in == 0 ? 64 : mps_in;
    _msc.mps_out = mps_out == 0 ? 64 : mps_out;
    _msc.tag = 1;

    if (bsp_usb_bulk_open(dev, ep_in, _msc.mps_in) != 0 ||
            bsp_usb_bulk_open(dev, ep_out, _msc.mps_out) != 0) {
        klog("bsp_usb: msc endpoint setup failed in=%02x out=%02x\n",
                ep_in, ep_out);
        memset(&_msc, 0, sizeof(_msc));
        return -1;
    }

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_OPCODE_INQUIRY;
    cdb[4] = sizeof(inquiry);
    memset(inquiry, 0, sizeof(inquiry));
    if (usb_msc_command(cdb, 6, true, inquiry, sizeof(inquiry)) != 0) {
        klog("bsp_usb: msc inquiry_failed addr=%u\n", dev->addr);
        memset(&_msc, 0, sizeof(_msc));
        return -1;
    }
    /* media may need a spin-up/debounce window after plug-in */
    {
        int ready = -1;
        for (int i = 0; i < 20; ++i) {
            ready = usb_msc_test_unit_ready();
            if (ready == 0) {
                break;
            }
            usleep(100000);
        }
        if (ready != 0) {
            klog("bsp_usb: msc not_ready addr=%u\n", dev->addr);
            memset(&_msc, 0, sizeof(_msc));
            return -1;
        }
    }

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_OPCODE_READ_CAPACITY10;
    memset(capacity, 0, sizeof(capacity));
    if (usb_msc_command(cdb, 10, true, capacity, sizeof(capacity)) != 0) {
        klog("bsp_usb: msc read_capacity_failed addr=%u\n", dev->addr);
        memset(&_msc, 0, sizeof(_msc));
        return -1;
    }
    /* READ CAPACITY returns the last LBA; sector_count is last_lba + 1 */
    _msc.sector_count = be32(capacity) + 1u;
    _msc.sector_size = be32(capacity + 4);
    if (_msc.sector_size != USB_MSC_SECTOR_SIZE) {
        klog("bsp_usb: msc unsupported_sector_size=%u addr=%u\n",
                _msc.sector_size, dev->addr);
        memset(&_msc, 0, sizeof(_msc));
        return -1;
    }
    _msc.ready = true;

    /* auto-mount the FAT32 volume: spawn a fat32fsd bound to this device,
       deferred to bsp_usb_poll (see _msc_mount_pending) */
    _msc_mount_pending = true;
    return 0;
}

/* fork the fat32fsd mount daemon for a claimed MSC device; called from
   bsp_usb_poll with /dev/hid0 guaranteed registered */
static void msc_mount_spawn(void) {
    fsinfo_t mnt_info;
    if (vfs_get_by_name("/mnt/udisk0", &mnt_info) == 0) {
        klog("bsp_usb: msc mount busy, /mnt/udisk0 still mounted\n");
        return;
    }
    int pid = fork();
    if (pid == 0) {
        proc_detach();
        if (proc_exec("/drivers/q8b/usbfat32fsd -u /dev/hid0 /mnt/udisk0") != 0) {
            exit(-1);
        }
    }
    else if (pid > 0) {
        _msc.child_pid = pid;
    }
    else {
        klog("bsp_usb: msc mount_fork_failed\n");
    }
}

/* Scan the config descriptor for the first bulk-only mass-storage interface. */
int bsp_usb_msc_probe(bsp_usb_dev_t* dev, const uint8_t* cfg, int cfg_len) {
    const usb_iface_desc_t* msc_iface = NULL;
    uint8_t ep_in = 0, ep_out = 0;
    uint16_t mps_in = 0, mps_out = 0;

    if (dev == NULL || cfg == NULL || _msc.claimed || _msc.ready) {
        return -1;
    }

    for (int off = 0; off + 2 <= cfg_len; ) {
        uint8_t len = cfg[off];
        uint8_t type = cfg[off + 1];

        if (len < 2 || off + len > cfg_len) {
            break;
        }
        if (type == USB_DESC_INTERFACE && len >= sizeof(usb_iface_desc_t)) {
            const usb_iface_desc_t* iface = (const usb_iface_desc_t*)(cfg + off);
            if (iface->bInterfaceClass == USB_CLASS_MSC &&
                    iface->bInterfaceProtocol == USB_MSC_PROTO_BBB &&
                    (iface->bInterfaceSubClass == USB_MSC_SUBCLASS_SCSI ||
                     iface->bInterfaceSubClass == USB_MSC_SUBCLASS_UFI)) {
                if (msc_iface == NULL) {
                    msc_iface = iface;
                    ep_in = 0;
                    ep_out = 0;
                }
            }
            else {
                /* endpoints belong to the preceding interface only */
                msc_iface = NULL;
            }
        }
        else if (type == USB_DESC_ENDPOINT && msc_iface != NULL &&
                len >= sizeof(usb_ep_desc_t)) {
            const usb_ep_desc_t* ep = (const usb_ep_desc_t*)(cfg + off);
            if ((ep->bmAttributes & 0x3u) == USB_ENDPOINT_XFER_BULK) {
                if ((ep->bEndpointAddress & USB_ENDPOINT_IN) != 0) {
                    ep_in = ep->bEndpointAddress;
                    mps_in = (uint16_t)(ep->wMaxPacketSize & 0x07FFu);
                }
                else {
                    ep_out = ep->bEndpointAddress;
                    mps_out = (uint16_t)(ep->wMaxPacketSize & 0x07FFu);
                }
            }
        }
        off += len;
    }

    if (msc_iface == NULL || ep_in == 0 || ep_out == 0) {
        return -1;
    }
    return msc_attach(dev, msc_iface->bInterfaceNumber,
            ep_in, ep_out, mps_in, mps_out);
}

/* device (or the tree it sits on) is gone: tell the fs daemon to exit
   and drop all MSC state. Safe to call when nothing is attached. */
void bsp_usb_msc_detach(bsp_usb_dev_t* dev) {
    if (!_msc.claimed || _msc.dev != dev) {
        return;
    }
    _msc_mount_pending = false;
    if (_msc.child_pid > 0) {
        /* fire-and-forget: the device is already gone, the daemon only
           needs the hint to unmount and exit */
        dev_cntl_by_pid(_msc.child_pid, USBFS_CMD_QUIT, NULL, NULL);
    }
    memset(&_msc, 0, sizeof(_msc));
}

bool bsp_usb_msc_attached(bsp_usb_dev_t* dev) {
    return _msc.claimed && _msc.dev == dev;
}

/* sector transport served to fat32fsd over FS_CMD_DEV_CNTL */
static int usb_msc_read_sectors(uint32_t sector, uint32_t count, proto_t* out) {
    uint8_t cdb[10];
    uint32_t len = count * USB_MSC_SECTOR_SIZE;
    uint8_t buf[USBMSC_MAX_SECTORS * USB_MSC_SECTOR_SIZE];

    if (len > sizeof(buf)) {
        return -1;
    }
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_OPCODE_READ10;
    put_be32(cdb + 2, sector);
    cdb[7] = (uint8_t)(count >> 8);
    cdb[8] = (uint8_t)(count & 0xFFu);
    if (usb_msc_command(cdb, 10, true, buf, len) != 0) {
        return -1;
    }
    PF->add(out, buf, len);
    return 0;
}

static int usb_msc_write_sectors(uint32_t sector, uint32_t count,
        const void* data, int32_t data_len) {
    uint8_t cdb[10];
    uint32_t len = count * USB_MSC_SECTOR_SIZE;

    if ((uint32_t)data_len < len) {
        return -1;
    }
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_OPCODE_WRITE10;
    put_be32(cdb + 2, sector);
    cdb[7] = (uint8_t)(count >> 8);
    cdb[8] = (uint8_t)(count & 0xFFu);
    return usb_msc_command(cdb, 10, false, (void*)data, len);
}

int bsp_usb_msc_cntl(vdevice_t* vdev, int from_pid, int cmd,
        proto_t* in, proto_t* out) {
    (void)vdev;
    (void)from_pid;

    switch (cmd) {
    case USBMSC_CMD_INFO:
        PF->addi(out, _msc.ready ? 1 : 0);
        PF->addi(out, _msc.sector_count);
        PF->addi(out, _msc.sector_size);
        return 0;
    case USBMSC_CMD_READ: {
        uint32_t sector = (uint32_t)proto_read_int(in);
        uint32_t count = (uint32_t)proto_read_int(in);
        if (!_msc.ready || count == 0 || count > USBMSC_MAX_SECTORS ||
                sector + count > _msc.sector_count) {
            return -1;
        }
        return usb_msc_read_sectors(sector, count, out);
    }
    case USBMSC_CMD_WRITE: {
        uint32_t sector = (uint32_t)proto_read_int(in);
        uint32_t count = (uint32_t)proto_read_int(in);
        int32_t sz = 0;
        void* data = proto_read(in, &sz);
        if (!_msc.ready || count == 0 || count > USBMSC_MAX_SECTORS ||
                sector + count > _msc.sector_count || data == NULL) {
            return -1;
        }
        return usb_msc_write_sectors(sector, count, data, sz);
    }
    case USBMSC_CMD_FLUSH:
        if (!_msc.ready) {
            return 0;
        }
        return (usb_msc_sync_cache() == 0) ? 0 : -1;
    default:
        return -1;
    }
}

/* Bluetooth HCI is a separate board milestone; keep the shared usbhostd
 * hooks total so ordinary composite devices can still enumerate. */
int bsp_usb_bt_probe(bsp_usb_dev_t* dev, uint16_t vid, uint16_t pid,
        const uint8_t* cfg, int cfg_len) {
    (void)dev; (void)vid; (void)pid; (void)cfg; (void)cfg_len;
    return -1;
}

void bsp_usb_bt_detach(bsp_usb_dev_t* dev) { (void)dev; }
bool bsp_usb_bt_attached(bsp_usb_dev_t* dev) { (void)dev; return false; }

int bsp_usb_bt_cntl(vdevice_t* vdev, int from_pid, int cmd,
        proto_t* in, proto_t* out) {
    (void)vdev; (void)from_pid; (void)cmd; (void)in; (void)out;
    return -1;
}
