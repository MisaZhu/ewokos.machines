/*
 * bsp_usb.c: raspi5 USB host abstraction on top of the RP1 xHCI driver.
 *
 * Owns the two RP1 xHCI controllers, flattens their root ports into one
 * 1-based port space and hands out opaque device handles backed by
 * xhci_dev_t slots. The usbhostd policy layer never touches xhci_*
 * directly.
 */
#include <bsp/bsp_usb.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <sysinfo.h>
#include <ewoksys/syscall.h>
#include <ewoksys/mmio.h>
#include <ewoksys/klog.h>
#include <arch/bcm2712/mmio.h>
#include <arch/bcm2712/rp1.h>
#include <arch/bcm2712/xhci.h>

/* rp1.dtsi: usb@200000 and usb@300000, xHCI caps at the window base */
#define RP1_XHCI0_OFF (PI5_RP1_WIN_OFF + 0x200000)
#define RP1_XHCI1_OFF (PI5_RP1_WIN_OFF + 0x300000)
#define BSP_USB_NUM_HCS 2
#define BSP_USB_MAX_DEVS 16

struct bsp_usb_dev {
    bool used;
    xhci_dev_t xdev;
};

static xhci_hc_t _hcs[BSP_USB_NUM_HCS];
static bsp_usb_dev_t _devs[BSP_USB_MAX_DEVS];
static bool _inited = false;

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

/* ---- init / poll ---- */

int bsp_usb_init(void) {
    sys_info_t sysinfo;
    bool main_mapped;
    bool rp1_mapped;
    int found = 0;

    if (_inited) {
        return 0;
    }
    _inited = true;
    memset(_devs, 0, sizeof(_devs));

    /* map the main MMIO window plus the RP1 window, like the other RP1
       users (uartd, i2c, spi) */
    syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&sysinfo);
    _mmio_base = sysinfo.mmio.v_base;
    main_mapped = syscall3(SYS_MEM_MAP,
            (ewokos_addr_t)sysinfo.mmio.v_base,
            (ewokos_addr_t)sysinfo.mmio.phy_base,
            (ewokos_addr_t)sysinfo.mmio.size) == sysinfo.mmio.v_base;
    /*
     * sys_mem_map() returns 0 and installs *nothing* when the request misses
     * check_mem_map_arch()'s whitelist, so an unchecked failure here would
     * turn the first CAPLENGTH read into a data abort that kills the daemon —
     * and a dead child leaves ipcserv spinning in ipc_ping() forever.
     */
    rp1_mapped = syscall3(SYS_MEM_MAP,
            _mmio_base + PI5_RP1_WIN_OFF,
            PI5_RP1_PHY,
            PI5_RP1_WIN_SIZE) != 0;

    /*
     * Never fail hard: ipcserv blocks in ipc_wait_ready() until the daemon
     * registers its mount point, so degrading to "no usb" is the only safe
     * path. Every entry point below is gated on hc->present.
     *
     * The RP1 window only decodes once the PCIe2 link is trained and the RP1
     * BARs are enabled; bcm2712_rp1_init() is idempotent and skips training
     * when the link is already up.
     */
    if (!main_mapped) {
        klog("bsp_usb: main mmio map failed, running without usb\n");
        return 0;
    }
    if (!rp1_mapped) {
        klog("bsp_usb: rp1 window map failed, running without usb\n");
        return 0;
    }
    if (bcm2712_rp1_init() != 0) {
        klog("bsp_usb: rp1 pcie init failed, running without usb\n");
        return 0;
    }
    if (xhci_dma_init() != 0) {
        klog("bsp_usb: dma_init_failed, running without usb\n");
        return 0;
    }
    if (xhci_init(&_hcs[0], 0, _mmio_base + RP1_XHCI0_OFF) == 0) {
        found++;
    }
    if (xhci_init(&_hcs[1], 1, _mmio_base + RP1_XHCI1_OFF) == 0) {
        found++;
    }
    if (found == 0) {
        klog("bsp_usb: no xhci controller found, running without usb\n");
    }
    return 0;
}

void bsp_usb_poll(void) {
    for (int i = 0; i < BSP_USB_NUM_HCS; ++i) {
        if (_hcs[i].present) {
            xhci_process_events(&_hcs[i]);
        }
    }
}

/* ---- root ports ---- */

int bsp_usb_root_port_count(void) {
    int count = 0;
    for (int i = 0; i < BSP_USB_NUM_HCS; ++i) {
        if (_hcs[i].present) {
            count += (int)_hcs[i].num_ports;
        }
    }
    return count;
}

bool bsp_usb_root_port_connected(int port) {
    int hc_port;
    xhci_hc_t* hc = port_to_hc(port, &hc_port);
    if (hc == NULL) {
        return false;
    }
    return xhci_port_connected(hc, hc_port);
}

int bsp_usb_root_port_reset(int port) {
    int hc_port;
    xhci_hc_t* hc = port_to_hc(port, &hc_port);
    int speed;
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
    return changes;
}

/* ---- device lifecycle ---- */

bsp_usb_dev_t* bsp_usb_device_attach(int root_port, int speed,
        bsp_usb_dev_t* parent_hub, int hub_port) {
    bsp_usb_dev_t* dev = NULL;
    xhci_hc_t* hc;
    int hc_port;

    for (int i = 0; i < BSP_USB_MAX_DEVS; ++i) {
        if (!_devs[i].used) {
            dev = &_devs[i];
            break;
        }
    }
    if (dev == NULL) {
        return NULL;
    }

    if (parent_hub != NULL) {
        hc = parent_hub->xdev.hc;
        hc_port = parent_hub->xdev.root_port;
    }
    else {
        hc = port_to_hc(root_port, &hc_port);
        if (hc == NULL) {
            return NULL;
        }
    }

    memset(dev, 0, sizeof(*dev));
    if (xhci_device_attach(hc, hc_port, speed_to_xhci(speed),
            parent_hub != NULL ? &parent_hub->xdev : NULL, hub_port,
            &dev->xdev) != 0) {
        return NULL;
    }
    dev->used = true;
    return dev;
}

void bsp_usb_device_detach(bsp_usb_dev_t* dev) {
    if (dev == NULL || !dev->used) {
        return;
    }
    xhci_device_detach(&dev->xdev);
    memset(dev, 0, sizeof(*dev));
}

int bsp_usb_device_update_mps0(bsp_usb_dev_t* dev, uint8_t mps0) {
    if (dev == NULL) {
        return -1;
    }
    return xhci_update_mps0(&dev->xdev, mps0);
}

int bsp_usb_device_configure_hub(bsp_usb_dev_t* dev, int num_ports) {
    if (dev == NULL) {
        return -1;
    }
    return xhci_configure_hub(&dev->xdev, num_ports);
}

/* ---- transfers ---- */

int bsp_usb_control_xfer(bsp_usb_dev_t* dev, const usb_setup_pkt_t* setup,
        void* data, bool dir_in) {
    if (dev == NULL) {
        return -1;
    }
    return xhci_control_xfer(&dev->xdev, setup, data, dir_in);
}

int bsp_usb_int_in_open(bsp_usb_dev_t* dev, uint8_t ep_addr, uint16_t mps,
        uint8_t interval) {
    if (dev == NULL) {
        return -1;
    }
    return xhci_int_in_open(&dev->xdev, ep_addr, mps, interval);
}

int bsp_usb_int_in_poll(bsp_usb_dev_t* dev, uint8_t ep_addr, void* buf,
        int size) {
    if (dev == NULL) {
        return -1;
    }
    return xhci_int_in_poll(&dev->xdev, ep_addr, buf, size);
}

/* the RP1 xHCI driver has no bulk endpoint support yet (no MSC consumer
   on raspi5 so far) */
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
 * and the input goes dead until replug.
 */
int bsp_usb_ep_clear_halt(bsp_usb_dev_t* dev, uint8_t ep_addr) {
    usb_setup_pkt_t setup;
    if (dev == NULL) {
        return -1;
    }
    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = USB_REQTYPE_STD_EP_OUT;
    setup.bRequest = USB_REQ_CLEAR_FEATURE;
    setup.wValue = USB_FEAT_ENDPOINT_HALT;
    setup.wIndex = ep_addr;
    return bsp_usb_control_xfer(dev, &setup, NULL, false);
}

/* ---- mass storage: not implemented on raspi5 (no bulk support in the
   RP1 xHCI driver yet), the shared usbhostd keeps these as no-ops ---- */

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
