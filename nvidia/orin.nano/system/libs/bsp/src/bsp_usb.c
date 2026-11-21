/* Tegra234 XUSB host glue for the Orin Nano devkit.
 *
 * bsp_xusb.c powers the XUSB block via BPMP and brings the USB2-1 pad up
 * (onboard hub -> USB-A ports).  This layer takes the xHCI register file
 * over with HCRST and reuses the shared EwokOS USB enumeration, HID, hub
 * and mass-storage policy.  Polled: no IRQ registration yet.
 */
#include <usb/bsp_usb.h>
#include <bsp/bsp_xusb.h>
#include <bsp/xhci.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <ewoksys/vfs.h>
#include <ewoksys/proc.h>
#include <ewoksys/syscall.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/klog.h>
#include <ewoksys/usbmsc.h>

#define BSP_USB_MAX_DEVS 16

struct bsp_usb_dev {
    bool used;
    xhci_dev_t xdev;
};

static bsp_usb_dev_t _devs[BSP_USB_MAX_DEVS];
static xhci_hc_t _xhc;
static bool _inited = false;
static bool _dirty = false;
static uint32_t _dev_count = 0;

typedef struct {
    bool ready;
    bool claimed;
    bool transport_failed;
    bsp_usb_dev_t* dev;
    uint8_t iface_num;
    uint8_t ep_in;
    uint8_t ep_out;
    uint16_t mps_in;
    uint16_t mps_out;
    uint32_t tag;
    uint32_t sector_count;
    uint32_t sector_size;
    int child_pid;
} usb_msc_t;

static usb_msc_t _msc;
static bool _msc_mount_pending = false;
static bool _msc_auto_mount = true;
static void msc_mount_spawn(void);

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
        return BSP_USB_SPEED_SUPER;
    case XHCI_SPEED_SUPER_PLUS:
        return BSP_USB_SPEED_SUPER_PLUS;
    default:
        return BSP_USB_SPEED_FULL;
    }
}

static int xhci_start(void) {
    ewokos_addr_t cap = bsp_xusb_init();
    if (cap == 0) {
        klog("bsp_usb: xusb controller bring-up failed\n");
        return -1;
    }
    if (xhci_init(&_xhc, 0, cap) != 0) {
        klog("bsp_usb: xhci init failed\n");
        return -1;
    }
    _dirty = true;
    klog("bsp_usb: xhci up root ports=%u\n", _xhc.num_ports);
    return 0;
}

/* ---- init / poll ---- */

int bsp_usb_init(void) {
    if (_inited)
        return 0;
    _inited = true;
    memset(_devs, 0, sizeof(_devs));
    memset(&_xhc, 0, sizeof(_xhc));
    if (xhci_dma_init() != 0) {
        klog("bsp_usb: xhci dma init failed\n");
        return 0;
    }
    xhci_start(); /* degrades to no controller, never fails */
    klog("bsp_usb: init ready root_ports=%d\n", bsp_usb_root_port_count());
    return 0;
}

int bsp_usb_reinit(void) {
    if (!_xhc.present)
        return -1;
    if (_msc.claimed && _msc.ready) {
        klog("bsp_usb: xhci reinit skipped while MSC is active\n");
        return -1;
    }
    if (_msc.child_pid > 0)
        dev_cntl_by_pid(_msc.child_pid, USBFS_CMD_QUIT, NULL, NULL);
    memset(_devs, 0, sizeof(_devs));
    memset(&_msc, 0, sizeof(_msc));
    _msc_mount_pending = false;
    _dev_count = 0;
    memset(&_xhc, 0, sizeof(_xhc));
    return xhci_start();
}

void bsp_usb_poll(void) {
    if (_msc_mount_pending && _msc_auto_mount) {
        _msc_mount_pending = false;
        msc_mount_spawn();
    }
    if (_xhc.present && (_dev_count > 0 || _dirty)) {
        _dirty = false;
        xhci_process_events(&_xhc);
    }
}

/* ---- root ports ---- */

int bsp_usb_root_port_count(void) {
    return _xhc.present ? (int)_xhc.num_ports : 0;
}

bool bsp_usb_root_port_connected(int port) {
    if (!_xhc.present || port < 1 || port > (int)_xhc.num_ports)
        return false;
    return xhci_port_connected(&_xhc, port);
}

uint32_t bsp_usb_root_port_changes(void) {
    if (!_xhc.present)
        return 0;
    return xhci_port_take_changes(&_xhc);
}

int bsp_usb_root_port_reset(int port) {
    if (!_xhc.present || port < 1 || port > (int)_xhc.num_ports)
        return -1;
    int speed = xhci_port_reset(&_xhc, port);
    if (speed < 0) {
        klog("bsp_usb: root port=%d reset failed\n", port);
        return -1;
    }
    klog("bsp_usb: root port=%d reset speed=%d\n", port,
            speed_from_xhci(speed));
    return speed_from_xhci(speed);
}

/* ---- device lifecycle ---- */

bsp_usb_dev_t* bsp_usb_device_attach(int root_port, int speed,
        bsp_usb_dev_t* parent_hub, int hub_port) {
    if (!_xhc.present)
        return NULL;
    bsp_usb_dev_t* dev = NULL;
    for (int i = 0; i < BSP_USB_MAX_DEVS; ++i) {
        if (!_devs[i].used) {
            dev = &_devs[i];
            break;
        }
    }
    if (dev == NULL)
        return NULL;

    /* a device behind a hub belongs to the controller owning the hub */
    int hc_port = parent_hub != NULL ? parent_hub->xdev.root_port : root_port;
    memset(dev, 0, sizeof(*dev));
    if (xhci_device_attach(&_xhc, hc_port, speed_to_xhci(speed),
            parent_hub != NULL ? &parent_hub->xdev : NULL, hub_port,
            &dev->xdev) != 0) {
        klog("bsp_usb: xhci attach failed port=%d speed=%d\n",
                root_port, speed);
        return NULL;
    }
    dev->used = true;
    _dev_count++;
    klog("bsp_usb: attached port=%d slot=%u speed=%d\n",
            root_port, dev->xdev.slot_id, speed);
    return dev;
}

void bsp_usb_device_detach(bsp_usb_dev_t* dev) {
    if (dev == NULL || !dev->used)
        return;
    xhci_device_detach(&dev->xdev);
    if (_dev_count > 0)
        _dev_count--;
    _dirty = true; /* teardown commands post events */
    memset(dev, 0, sizeof(*dev));
}

int bsp_usb_device_update_mps0(bsp_usb_dev_t* dev, uint8_t mps0) {
    if (dev == NULL || !dev->used)
        return -1;
    return xhci_update_mps0(&dev->xdev, mps0);
}

int bsp_usb_device_configure_hub(bsp_usb_dev_t* dev, int num_ports) {
    if (dev == NULL || !dev->used)
        return -1;
    return xhci_configure_hub(&dev->xdev, num_ports);
}

/* ---- transfers ---- */

int bsp_usb_control_xfer(bsp_usb_dev_t* dev, const usb_setup_pkt_t* setup,
        void* data, bool dir_in) {
    if (dev == NULL || !dev->used)
        return -1;
    return xhci_control_xfer(&dev->xdev, setup, data, dir_in);
}

int bsp_usb_int_in_open(bsp_usb_dev_t* dev, uint8_t ep_addr, uint16_t mps,
        uint8_t interval) {
    if (dev == NULL || !dev->used)
        return -1;
    return xhci_int_in_open(&dev->xdev, ep_addr, mps, interval);
}

int bsp_usb_int_in_poll(bsp_usb_dev_t* dev, uint8_t ep_addr, void* buf,
        int size) {
    if (dev == NULL || !dev->used)
        return -1;
    return xhci_int_in_poll(&dev->xdev, ep_addr, buf, size);
}

int bsp_usb_bulk_open(bsp_usb_dev_t* dev, uint8_t ep_addr, uint16_t mps) {
    if (dev == NULL || !dev->used)
        return -1;
    return xhci_bulk_open(&dev->xdev, ep_addr, mps);
}

int bsp_usb_bulk_xfer(bsp_usb_dev_t* dev, uint8_t ep_addr, void* data,
        int len, bool dir_in) {
    if (dev == NULL || !dev->used || len <= 0)
        return -1;
    return xhci_bulk_xfer(&dev->xdev, ep_addr, data, (uint32_t)len,
            dir_in, 2000u);
}

int bsp_usb_bulk_in_poll(bsp_usb_dev_t* dev, uint8_t ep_addr, void* buf,
        int size) {
    if (dev == NULL || !dev->used)
        return -1;
    return xhci_int_in_poll(&dev->xdev, ep_addr, buf, size);
}

int bsp_usb_ep_clear_halt(bsp_usb_dev_t* dev, uint8_t ep_addr) {
    if (dev == NULL || !dev->used)
        return -1;
    usb_setup_pkt_t setup;
    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = USB_REQTYPE_STD_EP_OUT;
    setup.bRequest = USB_REQ_CLEAR_FEATURE;
    setup.wValue = USB_FEAT_ENDPOINT_HALT;
    setup.wIndex = ep_addr;
    return bsp_usb_control_xfer(dev, &setup, NULL, false);
}

/* ---------------- mass storage (bulk-only transport) ---------------- */

#define MSC_BULK_TIMEOUT_MS 800u
#define MSC_CBW_TIMEOUT_MS 500u

void bsp_usb_set_msc_auto_mount(bool enabled) {
    _msc_auto_mount = enabled;
    if (!enabled)
        _msc_mount_pending = false;
    else if (_msc.ready && _msc.child_pid <= 0)
        _msc_mount_pending = true;
}

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

static int msc_bulk_xfer(bool dir_in, uint8_t ep_addr, void* data,
        uint32_t len, uint32_t timeout_ms) {
    return xhci_bulk_xfer(&_msc.dev->xdev, ep_addr, data, len,
            dir_in, timeout_ms);
}

static bool msc_link_present(void) {
    if (!_msc.claimed || _msc.dev == NULL || !_msc.dev->used)
        return false;
    return xhci_port_connected(_msc.dev->xdev.hc, _msc.dev->xdev.root_port);
}

static void msc_recover(void) {
    usb_setup_pkt_t setup;

    if (!msc_link_present()) {
        _msc.transport_failed = true;
        _msc.ready = false;
        return;
    }
    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = USB_REQTYPE_CLASS_IFACE_OUT;
    setup.bRequest = 0xFF; /* bulk-only mass storage reset */
    setup.wIndex = _msc.iface_num;
    int reset_ret = bsp_usb_control_xfer(_msc.dev, &setup, NULL, false);

    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = USB_REQTYPE_STD_EP_OUT;
    setup.bRequest = USB_REQ_CLEAR_FEATURE;
    setup.wValue = USB_FEAT_ENDPOINT_HALT;
    setup.wIndex = _msc.ep_in;
    int in_ret = bsp_usb_control_xfer(_msc.dev, &setup, NULL, false);
    setup.wIndex = _msc.ep_out;
    int out_ret = bsp_usb_control_xfer(_msc.dev, &setup, NULL, false);

    if (reset_ret != 0 || in_ret != 0 || out_ret != 0) {
        klog("bsp_usb: msc recovery reset=%d clear_in=%d clear_out=%d\n",
                reset_ret, in_ret, out_ret);
        _msc.transport_failed = true;
        _msc.ready = false;
    }
}

static usbmsc_stats_t _msc_stats;
static uint32_t _msc_depth;

static int usb_msc_command(const uint8_t* cdb, uint8_t cdb_len, bool dir_in,
        void* data, uint32_t data_len) {
    usb_cbw_t cbw;
    usb_csw_t csw;
    uint8_t* payload = NULL;
    uint32_t tag;
    int ret = -1;
    const uint32_t lba = (cdb_len >= 10) ? be32(cdb + 2) : 0;
    const uint32_t blocks = (cdb_len >= 10) ?
            (((uint32_t)cdb[7] << 8) | cdb[8]) : 0;

    if (!_msc.claimed || _msc.transport_failed || _msc.dev == NULL ||
            cdb_len > 16)
        return -1;
    if (data_len > 0) {
        payload = (uint8_t*)malloc(data_len);
        if (payload == NULL)
            return -1;
        if (!dir_in)
            memcpy(payload, data, data_len);
    }

    uint64_t start = 0, end = 0;
    kernel_tic_nsec(&start);
    _msc_stats.commands++;
    _msc_stats.bytes += data_len;
    if (++_msc_depth > _msc_stats.max_depth)
        _msc_stats.max_depth = _msc_depth;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (_msc.transport_failed || !msc_link_present()) {
            _msc.transport_failed = true;
            _msc.ready = false;
            goto out;
        }
        tag = ++_msc.tag;
        if (tag == 0)
            tag = ++_msc.tag;
        memset(&cbw, 0, sizeof(cbw));
        cbw.dCBWSignature = USB_MSC_CBW_SIG;
        cbw.dCBWTag = tag;
        cbw.dCBWDataTransferLength = data_len;
        cbw.bmCBWFlags = dir_in ? 0x80u : 0x00u;
        cbw.bCBWLUN = 0;
        cbw.bCBWCBLength = cdb_len;
        memcpy(cbw.CBWCB, cdb, cdb_len);

        if (msc_bulk_xfer(false, _msc.ep_out, &cbw, sizeof(cbw),
                MSC_CBW_TIMEOUT_MS) != (int)sizeof(cbw)) {
            klog("bsp_usb: msc op=%02x lba=%u len=%u cbw out failed\n",
                    cdb[0], lba, data_len);
            if (!msc_link_present()) {
                _msc.transport_failed = true;
                _msc.ready = false;
                goto out;
            }
            msc_recover();
            continue;
        }
        if (data_len > 0) {
            int xret = msc_bulk_xfer(dir_in,
                    dir_in ? _msc.ep_in : _msc.ep_out,
                    payload, data_len, MSC_BULK_TIMEOUT_MS);
            if (xret != (int)data_len) {
                klog("bsp_usb: msc op=%02x lba=%u blocks=%u data %s failed got=%d\n",
                        cdb[0], lba, blocks, dir_in ? "in" : "out", xret);
                if (!msc_link_present()) {
                    _msc.transport_failed = true;
                    _msc.ready = false;
                    goto out;
                }
                msc_recover();
                continue;
            }
        }
        if (msc_bulk_xfer(true, _msc.ep_in, &csw, sizeof(csw),
                MSC_BULK_TIMEOUT_MS) != (int)sizeof(csw)) {
            klog("bsp_usb: msc op=%02x lba=%u csw in failed\n", cdb[0], lba);
            if (!msc_link_present()) {
                _msc.transport_failed = true;
                _msc.ready = false;
                goto out;
            }
            msc_recover();
            continue;
        }
        if (csw.dCSWSignature != USB_MSC_CSW_SIG || csw.dCSWTag != tag) {
            klog("bsp_usb: msc op=%02x bad csw sig=%08x tag=%u want=%u\n",
                    cdb[0], csw.dCSWSignature, csw.dCSWTag, tag);
            if (!msc_link_present()) {
                _msc.transport_failed = true;
                _msc.ready = false;
                goto out;
            }
            msc_recover();
            continue;
        }
        if (csw.bCSWStatus != 0) {
            klog("bsp_usb: msc op=%02x lba=%u status=%u residue=%u\n",
                    cdb[0], lba, csw.bCSWStatus, csw.dCSWDataResidue);
            ret = -1;
            goto out;
        }
        if (dir_in && data != NULL && data_len > 0)
            memcpy(data, payload, data_len);
        ret = 0;
        goto out;
    }

out:
    kernel_tic_nsec(&end);
    _msc_stats.service_us += (uint32_t)((end - start) / 1000);
    _msc_stats.errors += ret != 0;
    _msc_depth--;
    free(payload);
    return ret;
}

static int usb_msc_test_unit_ready(void) {
    uint8_t cdb[6];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_OPCODE_TEST_UNIT_READY;
    return usb_msc_command(cdb, 6, false, NULL, 0);
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
        klog("bsp_usb: msc inquiry_failed\n");
        memset(&_msc, 0, sizeof(_msc));
        return -1;
    }
    {
        int ready = -1;
        for (int i = 0; i < 20; ++i) {
            ready = usb_msc_test_unit_ready();
            if (ready == 0)
                break;
            usleep(100000);
        }
        if (ready != 0) {
            klog("bsp_usb: msc not_ready\n");
            memset(&_msc, 0, sizeof(_msc));
            return -1;
        }
    }

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_OPCODE_READ_CAPACITY10;
    memset(capacity, 0, sizeof(capacity));
    if (usb_msc_command(cdb, 10, true, capacity, sizeof(capacity)) != 0) {
        klog("bsp_usb: msc read_capacity_failed\n");
        memset(&_msc, 0, sizeof(_msc));
        return -1;
    }
    _msc.sector_count = be32(capacity) + 1u;
    _msc.sector_size = be32(capacity + 4);
    if (_msc.sector_size != USB_MSC_SECTOR_SIZE) {
        klog("bsp_usb: msc unsupported_sector_size=%u\n", _msc.sector_size);
        memset(&_msc, 0, sizeof(_msc));
        return -1;
    }
    _msc.ready = true;
    klog("bsp_usb: msc ready sectors=%u size=%u\n",
            _msc.sector_count, _msc.sector_size);
    _msc_mount_pending = true;
    return 0;
}

static void msc_mount_spawn(void) {
    fsinfo_t mnt_info;
    if (vfs_get_by_name("/mnt/udisk0", &mnt_info) == 0) {
        klog("bsp_usb: msc mount busy, /mnt/udisk0 still mounted\n");
        return;
    }
    int pid = fork();
    if (pid == 0) {
        proc_detach();
        if (proc_exec("/drivers/orin.nano/usbfat32fsd -u /dev/usb0 /mnt/udisk0") != 0)
            exit(-1);
    }
    else if (pid > 0) {
        _msc.child_pid = pid;
    }
    else {
        klog("bsp_usb: msc mount_fork_failed\n");
    }
}

int bsp_usb_msc_probe(bsp_usb_dev_t* dev, const uint8_t* cfg, int cfg_len) {
    const usb_iface_desc_t* msc_iface = NULL;
    uint8_t ep_in = 0, ep_out = 0;
    uint16_t mps_in = 0, mps_out = 0;

    if (dev == NULL || cfg == NULL || _msc.claimed || _msc.ready)
        return -1;

    for (int off = 0; off + 2 <= cfg_len; ) {
        uint8_t len = cfg[off];
        uint8_t type = cfg[off + 1];
        if (len < 2 || off + len > cfg_len)
            break;
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

    if (msc_iface == NULL || ep_in == 0 || ep_out == 0)
        return -1;
    return msc_attach(dev, msc_iface->bInterfaceNumber,
            ep_in, ep_out, mps_in, mps_out);
}

void bsp_usb_msc_detach(bsp_usb_dev_t* dev) {
    if (!_msc.claimed || _msc.dev != dev)
        return;
    _msc_mount_pending = false;
    if (_msc.child_pid > 0)
        dev_cntl_by_pid(_msc.child_pid, USBFS_CMD_QUIT, NULL, NULL);
    memset(&_msc, 0, sizeof(_msc));
}

bool bsp_usb_msc_attached(bsp_usb_dev_t* dev) {
    return _msc.claimed && _msc.dev == dev;
}

static int usb_msc_read_sectors(uint32_t sector, uint32_t count, proto_t* out) {
    uint8_t cdb[10];
    uint32_t len = count * USB_MSC_SECTOR_SIZE;
    uint8_t buf[USBMSC_MAX_SECTORS * USB_MSC_SECTOR_SIZE];

    if (len > sizeof(buf))
        return -1;
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_OPCODE_READ10;
    put_be32(cdb + 2, sector);
    cdb[7] = (uint8_t)(count >> 8);
    cdb[8] = (uint8_t)(count & 0xFFu);
    if (usb_msc_command(cdb, 10, true, buf, len) != 0)
        return -1;
    PF->add(out, buf, len);
    return 0;
}

static int usb_msc_write_sectors(uint32_t sector, uint32_t count,
        const void* data, int32_t data_len) {
    uint8_t cdb[10];
    uint32_t len = count * USB_MSC_SECTOR_SIZE;

    if ((uint32_t)data_len < len)
        return -1;
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
    case USBMSC_CMD_STATS: {
        usbmsc_stats_t stats = _msc_stats;
        xhci_get_wait_stats(&stats);
        PF->add(out, &stats, sizeof(stats));
        return 0;
    }
    case USBMSC_CMD_AUTOMOUNT:
        bsp_usb_set_msc_auto_mount(proto_read_int(in) != 0);
        return 0;
    case USBMSC_CMD_INFO:
        PF->addi(out, _msc.ready ? 1 : 0);
        PF->addi(out, _msc.sector_count);
        PF->addi(out, _msc.sector_size);
        return 0;
    case USBMSC_CMD_READ: {
        uint32_t sector = (uint32_t)proto_read_int(in);
        uint32_t count = (uint32_t)proto_read_int(in);
        if (!_msc.ready || count == 0 || count > USBMSC_MAX_SECTORS ||
                sector + count > _msc.sector_count)
            return -1;
        return usb_msc_read_sectors(sector, count, out);
    }
    case USBMSC_CMD_WRITE: {
        uint32_t sector = (uint32_t)proto_read_int(in);
        uint32_t count = (uint32_t)proto_read_int(in);
        int32_t sz = 0;
        void* data = proto_read(in, &sz);
        if (!_msc.ready || count == 0 || count > USBMSC_MAX_SECTORS ||
                sector + count > _msc.sector_count || data == NULL)
            return -1;
        return usb_msc_write_sectors(sector, count, data, sz);
    }
    case USBMSC_CMD_FLUSH:
        return 0;
    default:
        return -1;
    }
}

/* ---- bluetooth: RTL8822CE transport is a follow-up (task #3) ---- */

int bsp_usb_bt_probe(bsp_usb_dev_t* dev, uint16_t vid, uint16_t pid,
        const uint8_t* cfg, int cfg_len) {
    (void)dev; (void)vid; (void)pid; (void)cfg; (void)cfg_len;
    return -1;
}

void bsp_usb_bt_detach(bsp_usb_dev_t* dev) {
    (void)dev;
}

bool bsp_usb_bt_attached(bsp_usb_dev_t* dev) {
    (void)dev;
    return false;
}

int bsp_usb_bt_cntl(vdevice_t* vdev, int from_pid, int cmd,
        proto_t* in, proto_t* out) {
    (void)vdev; (void)from_pid; (void)cmd; (void)in; (void)out;
    return -1;
}