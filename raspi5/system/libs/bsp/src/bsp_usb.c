/*
 * bsp_usb.c: raspi5 USB host abstraction.
 *
 * Owns two different host controllers at once:
 *
 *   - the two RP1 xHCI controllers (usb@200000 / usb@300000 inside the RP1
 *     window), which serve the Pi 5's own USB3/USB2 receptacles;
 *   - the BCM2712 SoC DWC2 at 0x10_00480000, which is what CM5 exposes as its
 *     "built-in USB 2.0 hub".
 *
 * The second one is not optional: a CM5 carrier such as the uConsole wires its
 * onboard GL850 hub — keyboard and trackball behind it — to the SoC USB2 pins,
 * not to RP1, and its config.txt carries dtoverlay=dwc2,dr_mode=host under [all]
 * precisely because that is the controller those ports live on. Driving only
 * xHCI therefore leaves the uConsole with no input devices at all.
 *
 * Both are flattened into one 1-based root-port space: the xHCI ports keep
 * their existing indices and the DWC2 root port is appended last, so its index
 * stays put while xHCI controllers come and go. Device handles carry a flag
 * that says which controller owns them; the usbhostd policy layer never sees
 * either xhci_* or dwc2_*.
 */
#include <usb/bsp_usb.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <sysinfo.h>
#include <ewoksys/syscall.h>
#include <ewoksys/mmio.h>
#include <ewoksys/proc.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/klog.h>
#include <arch/bcm2712/mmio.h>
#include <arch/bcm2712/rp1.h>
#include <arch/bcm2712/xhci.h>
#include <arch/bcm2712/dwc2.h>

/* rp1.dtsi: usb@200000 and usb@300000, xHCI caps at the window base */
#define RP1_XHCI0_OFF (PI5_RP1_WIN_OFF + 0x200000)
#define RP1_XHCI1_OFF (PI5_RP1_WIN_OFF + 0x300000)
#define BSP_USB_NUM_HCS 2
#define BSP_USB_MAX_DEVS 16

/* interrupt-IN pacing on the polled DWC2: the shared layer passes bInterval
   through; a polled controller honours it as a floor and stretches it while
   the endpoint keeps NAKing */
#define BSP_USB_INT_MIN_INTERVAL_MS 8u
#define BSP_USB_INT_MAX_INTERVAL_MS 40u
#define BSP_USB_INT_IDLE_STRETCH_2 64u
#define BSP_USB_INT_IDLE_STRETCH_4 256u

/* the dwc2 port can latch a dead not_enabled state that only a full core
   re-init clears; reinit after this many consecutive reset fails */
#define BSP_USB_RESET_FAIL_REINIT 6u

struct bsp_usb_dev {
    bool used;
    /* true: owned by the SoC DWC2, the xhci_dev_t below is unused and the
       polled state under it is what the transfers run on */
    bool on_dwc2;
    xhci_dev_t xdev;
    uint8_t addr;
    bool low_speed;
    uint8_t ctrl_mps;
    /* single polled interrupt-IN endpoint (the HID report endpoint) */
    uint8_t int_ep_addr;
    uint16_t int_mps;
    uint8_t int_toggle;
    uint32_t int_interval_ms;
    uint64_t int_next_ms;
    uint32_t int_idle_polls;
};

static xhci_hc_t _hcs[BSP_USB_NUM_HCS];
static bsp_usb_dev_t _devs[BSP_USB_MAX_DEVS];
static bool _inited = false;
/*
 * Per-controller activity: bsp_usb_poll() skips a controller that holds no
 * attached device and is not marked dirty. With an idle mouse port the
 * usbhostd loop would otherwise walk both event rings hundreds of times
 * per second for nothing. Dirty is set on detach so the command events a
 * device teardown posts are drained once afterwards. Hot-plug detection
 * does not depend on this path: usb_scan_root_ports() reads port status
 * straight from PORTSC.
 */
static uint32_t _hc_dev_count[BSP_USB_NUM_HCS];
static bool _hc_dirty[BSP_USB_NUM_HCS];

/* SoC DWC2 state: present only when dwc2_init() found a live core */
static bool _dwc2_present = false;
static uint8_t _dwc2_next_address = 2;
static bool _dwc2_prev_connected = false;
static uint32_t _dwc2_reset_fail_streak = 0;
/* high-speed probe in progress: set when the last root-port reset came back
   HIGH, cleared when the first transaction moves data or the port drops to
   FS/LS. A pure-TXERR failure while it is set means the 480Mbps data path is
   dead on this board -> latch FS/LS-only */
static bool _dwc2_pending_hs = false;
static bool _dwc2_fs_only = false;

static int hc_index(xhci_hc_t* hc) {
    for (int i = 0; i < BSP_USB_NUM_HCS; ++i) {
        if (&_hcs[i] == hc) {
            return i;
        }
    }
    return -1;
}

/* ---- flat root port numbering across the present controllers ---- */

static xhci_hc_t* port_to_hc(int port, int* hc_port) {
    for (int i = 0; i < BSP_USB_NUM_HCS; ++i) {
        if (!_hcs[i].present) {
            continue;
        }
        if (port <= (int)_hcs[i].num_ports) {
            if (hc_port != NULL) {
                *hc_port = port;
            }
            return &_hcs[i];
        }
        port -= (int)_hcs[i].num_ports;
    }
    return NULL;
}

static int xhci_port_count(void) {
    int count = 0;
    for (int i = 0; i < BSP_USB_NUM_HCS; ++i) {
        if (_hcs[i].present) {
            count += (int)_hcs[i].num_ports;
        }
    }
    return count;
}

/* the DWC2 root port always sits last, so xHCI presence changes never move it */
static int dwc2_port_index(void) {
    return _dwc2_present ? (xhci_port_count() + 1) : -1;
}

static bool port_is_dwc2(int port) {
    return _dwc2_present && port == dwc2_port_index();
}

static int speed_to_xhci(int speed) {
    switch (speed) {
    case BSP_USB_SPEED_LOW:
        return XHCI_SPEED_LOW;
    case BSP_USB_SPEED_FULL:
        return XHCI_SPEED_FULL;
    case BSP_USB_SPEED_HIGH:
        return XHCI_SPEED_HIGH;
    default:
        return XHCI_SPEED_FULL;
    }
}

static int speed_from_xhci(int speed) {
    switch (speed) {
    case XHCI_SPEED_LOW:
        return BSP_USB_SPEED_LOW;
    case XHCI_SPEED_HIGH:
    case XHCI_SPEED_SUPER:
        return BSP_USB_SPEED_HIGH;
    default:
        return BSP_USB_SPEED_FULL;
    }
}

/* ---- controller bring-up ---- */

/*
 * Never fail hard: ipcserv blocks in ipc_wait_ready() until the daemon
 * registers its mount point, so degrading to "no xhci" is the only safe path.
 * Every entry point below is gated on hc->present.
 *
 * The RP1 window only decodes once the PCIe2 link is trained and the RP1 BARs
 * are enabled; bcm2712_rp1_init() is idempotent and skips training when the
 * link is already up.
 *
 * sys_mem_map() returns 0 and installs *nothing* when the request misses
 * check_mem_map_arch()'s whitelist, so an unchecked failure here would turn
 * the first CAPLENGTH read into a data abort that kills the daemon — and a
 * dead child leaves ipcserv spinning in ipc_ping() forever.
 */
static int xhci_bring_up(void) {
    int found = 0;

    if (syscall3(SYS_MEM_MAP,
            _mmio_base + PI5_RP1_WIN_OFF,
            PI5_RP1_PHY,
            PI5_RP1_WIN_SIZE) == 0) {
        klog("bsp_usb: rp1 window map failed, no xhci\n");
        return 0;
    }
    if (bcm2712_rp1_init() != 0) {
        klog("bsp_usb: rp1 pcie init failed, no xhci\n");
        return 0;
    }
    if (xhci_dma_init() != 0) {
        klog("bsp_usb: xhci dma init failed, no xhci\n");
        return 0;
    }
    if (xhci_init(&_hcs[0], 0, _mmio_base + RP1_XHCI0_OFF) == 0) {
        found++;
    }
    if (xhci_init(&_hcs[1], 1, _mmio_base + RP1_XHCI1_OFF) == 0) {
        found++;
    }
    return found;
}

/* ---- init / poll ---- */

int bsp_usb_init(void) {
    sys_info_t sysinfo;
    int found = 0;

    if (_inited) {
        return 0;
    }
    _inited = true;
    memset(_devs, 0, sizeof(_devs));
    memset(_hc_dev_count, 0, sizeof(_hc_dev_count));
    memset(_hc_dirty, 0, sizeof(_hc_dirty));

    /* map the main MMIO window, like the other RP1 users (uartd, i2c, spi);
       both controllers need it (the VPU mailbox lives in it too) */
    syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&sysinfo);
    _mmio_base = sysinfo.mmio.v_base;
    if (syscall3(SYS_MEM_MAP,
            (ewokos_addr_t)sysinfo.mmio.v_base,
            (ewokos_addr_t)sysinfo.mmio.phy_base,
            (ewokos_addr_t)sysinfo.mmio.size) != sysinfo.mmio.v_base) {
        klog("bsp_usb: main mmio map failed, running without usb\n");
        return 0;
    }

    found = xhci_bring_up();

    /*
     * The SoC DWC2 is probed independently of RP1: on a CM5 carrier it is the
     * only controller the onboard hub hangs off, and a plain Pi 5 — where
     * those USB2 pins are not wired out — simply fails the probe and keeps
     * running on xHCI alone.
     */
    if (dwc2_init() == 0) {
        _dwc2_present = true;
        found++;
    }

    if (found == 0) {
        klog("bsp_usb: no usb controller found, running without usb\n");
    }
    return 0;
}

static void dwc2_state_reset(void) {
    _dwc2_next_address = 2;
    _dwc2_prev_connected = false;
    _dwc2_reset_fail_streak = 0;
    _dwc2_pending_hs = false;
    _dwc2_fs_only = false;
    dwc2_force_fs_only(false);
}

int bsp_usb_reinit(void) {
    int found = 0;

    if (!_inited) {
        return -1;
    }
    /* HCRST drops every slot context and a dwc2 core re-init forgets every
       address: all attached devices become stale, so forget the local handles
       too. The policy layer re-enumerates the tree from scratch afterwards. */
    memset(_devs, 0, sizeof(_devs));
    memset(_hc_dev_count, 0, sizeof(_hc_dev_count));
    for (int i = 0; i < BSP_USB_NUM_HCS; ++i) {
        _hc_dirty[i] = true;
    }
    if (xhci_init(&_hcs[0], 0, _mmio_base + RP1_XHCI0_OFF) == 0) {
        found++;
    }
    if (xhci_init(&_hcs[1], 1, _mmio_base + RP1_XHCI1_OFF) == 0) {
        found++;
    }
    if (_dwc2_present) {
        dwc2_state_reset();
        if (dwc2_reinit() == 0) {
            found++;
        }
        else {
            klog("bsp_usb: dwc2 core reinit failed\n");
        }
    }
    if (found == 0) {
        klog("bsp_usb: reinit found no usb controller\n");
        return -1;
    }
    return 0;
}

void bsp_usb_poll(void) {
    for (int i = 0; i < BSP_USB_NUM_HCS; ++i) {
        if (_hcs[i].present && (_hc_dev_count[i] > 0 || _hc_dirty[i])) {
            _hc_dirty[i] = false;
            xhci_process_events(&_hcs[i]);
        }
    }
    /* the SoC DWC2 is polled on demand; it has no event ring to drain */
}

/* ---- root ports ---- */

int bsp_usb_root_port_count(void) {
    int count = xhci_port_count();
    if (_dwc2_present) {
        count++;
    }
    return count;
}

bool bsp_usb_root_port_connected(int port) {
    int hc_port;
    xhci_hc_t* hc;

    if (port_is_dwc2(port)) {
        return dwc2_port_connected();
    }
    hc = port_to_hc(port, &hc_port);
    if (hc == NULL) {
        return false;
    }
    return xhci_port_connected(hc, hc_port);
}

/*
 * DWC2 has no connect-change register, so the change bit is derived from the
 * connection state itself — and a disconnect is the moment to drop the
 * FS/LS-only latch: it only exists to skip HS on a link that already proved it
 * cannot move a single byte at 480Mbps, a freshly plugged device deserves one
 * high-speed probe again.
 */
static uint32_t dwc2_take_changes(void) {
    bool conn;
    uint32_t changed;

    if (!_dwc2_present || !dwc2_ready()) {
        return 0;
    }
    conn = dwc2_port_connected();
    changed = (conn != _dwc2_prev_connected) ? 1u : 0u;
    _dwc2_prev_connected = conn;
    if (!conn) {
        dwc2_state_reset();
    }
    dwc2_ack_port_change();
    return changed;
}

static int dwc2_port_reset(void) {
    int speed;

    if (!dwc2_ready()) {
        return -1;
    }
    _dwc2_next_address = 2;
    speed = dwc2_reset_port();
    if (speed < 0) {
        _dwc2_reset_fail_streak++;
        if (_dwc2_reset_fail_streak >= BSP_USB_RESET_FAIL_REINIT) {
            klog("bsp_usb: dwc2 core reinit after %u failed resets\n",
                    _dwc2_reset_fail_streak);
            _dwc2_reset_fail_streak = 0;
            (void)dwc2_reinit();
        }
        return -1;
    }
    _dwc2_reset_fail_streak = 0;
    _dwc2_pending_hs = (speed == BSP_USB_SPEED_HIGH);
    return speed;
}

int bsp_usb_root_port_reset(int port) {
    int hc_port;
    xhci_hc_t* hc;
    int speed;

    if (port_is_dwc2(port)) {
        return dwc2_port_reset();
    }
    hc = port_to_hc(port, &hc_port);
    if (hc == NULL) {
        return -1;
    }
    speed = xhci_port_reset(hc, hc_port);
    if (speed < 0) {
        return -1;
    }
    return speed_from_xhci(speed);
}

uint32_t bsp_usb_root_port_changes(void) {
    uint32_t changes = 0;
    int shift = 0;
    for (int i = 0; i < BSP_USB_NUM_HCS; ++i) {
        if (!_hcs[i].present) {
            continue;
        }
        changes |= xhci_port_take_changes(&_hcs[i]) << shift;
        shift += (int)_hcs[i].num_ports;
    }
    changes |= dwc2_take_changes() << shift;
    return changes;
}

/* ---- device lifecycle ---- */

/*
 * SET_ADDRESS on the polled controller: dwc2 has no slot contexts, so the bsp
 * layer owns the address space and keeps the per-device transaction state
 * (address, toggle, cadence) that a software-programmed channel needs.
 */
static int dwc2_device_attach(bsp_usb_dev_t* dev, int speed) {
    usb_setup_pkt_t setup;
    uint8_t addr;

    if (!dwc2_ready()) {
        return -1;
    }
    addr = _dwc2_next_address++;
    if (addr == 0 || addr > 126) {
        _dwc2_next_address = 2;
        addr = _dwc2_next_address++;
    }
    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = USB_REQTYPE_STD_OUT;
    setup.bRequest = USB_REQ_SET_ADDRESS;
    setup.wValue = addr;
    if (dwc2_control_xfer(0, speed == BSP_USB_SPEED_LOW, 8,
            &setup, NULL, false) < 0) {
        /* The port negotiated high speed but the device never answers a
           single SETUP (instant TXERR): the 480Mbps data path is dead on
           this board. Latch FS/LS-only so the next reset suppresses the
           chirp and the device enumerates at full speed, instead of
           reset-hammering until the chirp randomly fails. Re-init the
           core too: flipping FSLSSUPP while the port is still enabled in
           HS mode wedges the port-enable state machine. */
        if (_dwc2_pending_hs && !_dwc2_fs_only && dwc2_last_xfer_txerr()) {
            klog("bsp_usb: dwc2 hs data path dead (pure txerr), force fs-only mode\n");
            _dwc2_fs_only = true;
            dwc2_force_fs_only(true);
            (void)dwc2_reinit();
        }
        return -1;
    }
    proc_usleep(10000); /* USB spec: new address is valid after 2ms */

    dev->used = true;
    dev->on_dwc2 = true;
    dev->addr = addr;
    dev->low_speed = (speed == BSP_USB_SPEED_LOW);
    dev->ctrl_mps = 8;
    return 0;
}

bsp_usb_dev_t* bsp_usb_device_attach(int root_port, int speed,
        bsp_usb_dev_t* parent_hub, int hub_port) {
    bsp_usb_dev_t* dev = NULL;
    xhci_hc_t* hc = NULL;
    int hc_port = 0;
    bool on_dwc2;

    /*
     * Routing: a device behind a hub belongs to whichever controller owns the
     * hub, a root device to the controller that owns its port. dwc2 has no
     * split-transaction support, so an FS/LS device behind a high-speed hub on
     * it is unservable — the uConsole hub enumerates full speed, which is the
     * configuration raspix proves works, and the FS/LS-only latch below is
     * what keeps it there.
     */
    if (parent_hub != NULL) {
        on_dwc2 = parent_hub->on_dwc2;
        if (!on_dwc2) {
            hc = parent_hub->xdev.hc;
            hc_port = parent_hub->xdev.root_port;
        }
    }
    else if (port_is_dwc2(root_port)) {
        on_dwc2 = true;
    }
    else {
        on_dwc2 = false;
        hc = port_to_hc(root_port, &hc_port);
        if (hc == NULL) {
            return NULL;
        }
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

    memset(dev, 0, sizeof(*dev));
    if (on_dwc2) {
        if (dwc2_device_attach(dev, speed) != 0) {
            memset(dev, 0, sizeof(*dev));
            return NULL;
        }
        return dev;
    }

    if (xhci_device_attach(hc, hc_port, speed_to_xhci(speed),
            parent_hub != NULL ? &parent_hub->xdev : NULL, hub_port,
            &dev->xdev) != 0) {
        return NULL;
    }
    dev->used = true;
    int idx = hc_index(dev->xdev.hc);
    if (idx >= 0) {
        _hc_dev_count[idx]++;
    }
    return dev;
}

void bsp_usb_device_detach(bsp_usb_dev_t* dev) {
    if (dev == NULL || !dev->used) {
        return;
    }
    if (dev->on_dwc2) {
        memset(dev, 0, sizeof(*dev));
        return;
    }
    int idx = hc_index(dev->xdev.hc);
    xhci_device_detach(&dev->xdev);
    if (idx >= 0 && _hc_dev_count[idx] > 0) {
        _hc_dev_count[idx]--;
        /* the teardown commands post events; drain them once even if
           this was the controller's last device */
        _hc_dirty[idx] = true;
    }
    memset(dev, 0, sizeof(*dev));
}

int bsp_usb_device_update_mps0(bsp_usb_dev_t* dev, uint8_t mps0) {
    if (dev == NULL || !dev->used) {
        return -1;
    }
    if (dev->on_dwc2) {
        dev->ctrl_mps = mps0;
        return 0;
    }
    return xhci_update_mps0(&dev->xdev, mps0);
}

int bsp_usb_device_configure_hub(bsp_usb_dev_t* dev, int num_ports) {
    if (dev == NULL || !dev->used) {
        return -1;
    }
    if (dev->on_dwc2) {
        /* no TT bookkeeping needed: devices behind an FS hub are reached by
           their own address, FS/LS behind a HS hub is simply impossible here */
        return 0;
    }
    return xhci_configure_hub(&dev->xdev, num_ports);
}

/* ---- transfers ---- */

int bsp_usb_control_xfer(bsp_usb_dev_t* dev, const usb_setup_pkt_t* setup,
        void* data, bool dir_in) {
    if (dev == NULL || !dev->used) {
        return -1;
    }
    if (dev->on_dwc2) {
        return dwc2_control_xfer(dev->addr, dev->low_speed, dev->ctrl_mps,
                setup, data, dir_in);
    }
    return xhci_control_xfer(&dev->xdev, setup, data, dir_in);
}

int bsp_usb_int_in_open(bsp_usb_dev_t* dev, uint8_t ep_addr, uint16_t mps,
        uint8_t interval) {
    uint32_t iv;

    if (dev == NULL || !dev->used) {
        return -1;
    }
    if (!dev->on_dwc2) {
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

static int dwc2_int_in_poll(bsp_usb_dev_t* dev, uint8_t ep_addr, void* buf,
        int size) {
    uint64_t now;
    int ret;

    if (dev->int_ep_addr != ep_addr || dev->int_mps == 0) {
        return 0;
    }
    now = kernel_tic_ms(0);
    if (now < dev->int_next_ms) {
        return 0;
    }
    dev->int_next_ms = now + int_in_interval(dev);

    ret = dwc2_int_in_xfer(dev->addr, dev->low_speed, ep_addr & 0x0Fu,
            dev->int_mps, &dev->int_toggle, buf, (uint16_t)size);
    if (ret == DWC2_XFER_RETRY) {
        return -2; /* endpoint STALL: let the policy layer clear halt */
    }
    if (ret > 0) {
        /* real data arrived: restore the base cadence immediately */
        dev->int_idle_polls = 0;
        dev->int_next_ms = now + dev->int_interval_ms;
        return ret;
    }
    dev->int_idle_polls++;
    return (ret == 0) ? 0 : -1;
}

int bsp_usb_int_in_poll(bsp_usb_dev_t* dev, uint8_t ep_addr, void* buf,
        int size) {
    if (dev == NULL || !dev->used) {
        return -1;
    }
    if (dev->on_dwc2) {
        return dwc2_int_in_poll(dev, ep_addr, buf, size);
    }
    return xhci_int_in_poll(&dev->xdev, ep_addr, buf, size);
}

/* the RP1 xHCI driver has no bulk endpoint support yet (no MSC consumer on
   raspi5 so far); dwc2 can do bulk, but nothing spawns the fs daemon that
   would consume it here, so the generic API stays stubbed for both */
int bsp_usb_bulk_open(bsp_usb_dev_t* dev, uint8_t ep_addr, uint16_t mps) {
    (void)dev;
    (void)ep_addr;
    (void)mps;
    return -1;
}

int bsp_usb_bulk_xfer(bsp_usb_dev_t* dev, uint8_t ep_addr, void* data,
        int len, bool dir_in) {
    (void)dev;
    (void)ep_addr;
    (void)data;
    (void)len;
    (void)dir_in;
    return -1;
}

/*
 * A STALL leaves the halt latched on both sides: xhci_int_in_poll() already
 * did Reset Endpoint + Set TR Dequeue on the controller side, but the device
 * keeps returning STALL until its own halt feature is cleared (USB 2.0
 * 9.4.5). Without this the endpoint stalls forever after one protocol error
 * and the input goes dead until replug. On dwc2 the software toggle has to
 * restart at DATA0 too, or the next transaction DTERRs.
 */
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
    if (dev->on_dwc2 && dev->int_ep_addr == ep_addr) {
        dev->int_toggle = 0;
    }
    return ret;
}

/* ---- mass storage: not wired up on raspi5, the shared usbhostd keeps these
   as no-ops (see the bsp_usb_bulk_* note above) ---- */

int bsp_usb_msc_probe(bsp_usb_dev_t* dev, const uint8_t* cfg, int cfg_len) {
    (void)dev;
    (void)cfg;
    (void)cfg_len;
    return -1;
}

void bsp_usb_msc_detach(bsp_usb_dev_t* dev) {
    (void)dev;
}

bool bsp_usb_msc_attached(bsp_usb_dev_t* dev) {
    (void)dev;
    return false;
}

int bsp_usb_msc_cntl(vdevice_t* vdev, int from_pid, int cmd,
        proto_t* in, proto_t* out) {
    (void)vdev;
    (void)from_pid;
    (void)cmd;
    (void)in;
    (void)out;
    return -1;
}
