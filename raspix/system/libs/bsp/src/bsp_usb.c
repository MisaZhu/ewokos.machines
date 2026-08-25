/*
 * bsp_usb.c: raspix USB host abstraction on top of the polled DWC2 driver.
 *
 * One root port, one addressed device per slot, no split transactions:
 * FS/LS devices behind a high-speed hub cannot be served, and a board
 * whose 480Mbps data path is dead gets latched into FS/LS-only mode
 * (the state machine that used to live in usbhostd now sits here).
 *
 * Also owns the mass-storage policy for raspix: the bulk-only transport
 * and the fat32fsd auto-mount ride on dwc2's dedicated bulk channels and
 * are served through the bsp_usb_msc_* hooks of the shared usbhostd.
 */
#include <bsp/bsp_usb.h>
#include <arch/bcm283x/dwc2.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <ewoksys/mmio.h>
#include <ewoksys/vfs.h>
#include <ewoksys/proc.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/klog.h>
#include <ewoksys/usbmsc.h>

#define BSP_USB_MAX_DEVS 8

/* interrupt-IN pacing: the shared layer passes bInterval through; a
   polled controller honours it as a floor and stretches it while the
   endpoint keeps NAKing */
#define BSP_USB_INT_MIN_INTERVAL_MS 8u
#define BSP_USB_INT_MAX_INTERVAL_MS 40u
#define BSP_USB_INT_IDLE_STRETCH_2 64u
#define BSP_USB_INT_IDLE_STRETCH_4 256u

/* the dwc2 port can latch a dead not_enabled state that only a full
   core re-init clears; reinit after this many consecutive reset fails */
#define BSP_USB_RESET_FAIL_REINIT 6u

struct bsp_usb_dev {
    bool used;
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

static bsp_usb_dev_t _devs[BSP_USB_MAX_DEVS];
static uint8_t _next_address = 2;
static bool _prev_connected = false;
static uint32_t _reset_fail_streak = 0;
/* high-speed probe in progress: set when the last root-port reset came
   back HIGH, cleared when the first transaction moves data or the port
   drops to FS/LS. A pure-TXERR failure while it is set means the 480Mbps
   data path is dead on this board -> latch FS/LS-only */
static bool _pending_hs = false;
static bool _fs_only = false;

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

/* ---- init / poll ---- */

int bsp_usb_init(void) {
    ewokos_addr_t base = mmio_map();
    if (base == 0) {
        klog("bsp_usb: mmio_map failed, running without usb\n");
        return 0;
    }
    if (dwc2_init(base) != 0) {
        klog("bsp_usb: dwc2 init failed, running without usb\n");
    }
    return 0;
}

void bsp_usb_poll(void) {
    /* everything is polled on demand; nothing to drain */
}

/* ---- root port (single) ---- */

int bsp_usb_root_port_count(void) {
    return 1;
}

bool bsp_usb_root_port_connected(int port) {
    if (port != 1) {
        return false;
    }
    return dwc2_port_connected();
}

uint32_t bsp_usb_root_port_changes(void) {
    bool conn;
    uint32_t changed;

    if (!dwc2_ready()) {
        return 0;
    }
    conn = dwc2_port_connected();
    changed = (conn != _prev_connected) ? 1u : 0u;
    _prev_connected = conn;
    if (!conn) {
        /* a fresh attach gets one high-speed probe again: the latch only
           exists to skip HS on a link that already proved it cannot move
           a single byte at 480Mbps */
        _fs_only = false;
        dwc2_force_fs_only(false);
        _pending_hs = false;
        _reset_fail_streak = 0;
        _next_address = 2;
    }
    dwc2_ack_port_change();
    return changed;
}

int bsp_usb_root_port_reset(int port) {
    int speed;

    if (port != 1 || !dwc2_ready()) {
        return -1;
    }
    _next_address = 2;
    speed = dwc2_reset_port();
    if (speed < 0) {
        _reset_fail_streak++;
        if (_reset_fail_streak >= BSP_USB_RESET_FAIL_REINIT) {
            klog("bsp_usb: core reinit after %u failed resets\n",
                    _reset_fail_streak);
            _reset_fail_streak = 0;
            (void)dwc2_reinit();
        }
        return -1;
    }
    _reset_fail_streak = 0;
    _pending_hs = (speed == BSP_USB_SPEED_HIGH);
    return speed;
}

/* ---- device lifecycle ---- */

bsp_usb_dev_t* bsp_usb_device_attach(int root_port, int speed,
        bsp_usb_dev_t* parent_hub, int hub_port) {
    bsp_usb_dev_t* dev = NULL;
    usb_setup_pkt_t setup;
    uint8_t addr;

    (void)parent_hub;
    (void)hub_port; /* topology is irrelevant for polled channel xfers */
    if (root_port != 1 || !dwc2_ready()) {
        return NULL;
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

    addr = _next_address++;
    if (addr == 0 || addr > 126) {
        _next_address = 2;
        addr = _next_address++;
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
        if (_pending_hs && !_fs_only && dwc2_last_xfer_txerr()) {
            klog("bsp_usb: hs data path dead (pure txerr), force fs-only mode\n");
            _fs_only = true;
            dwc2_force_fs_only(true);
            (void)dwc2_reinit();
        }
        return NULL;
    }
    proc_usleep(10000); /* USB spec: new address is valid after 2ms */

    memset(dev, 0, sizeof(*dev));
    dev->used = true;
    dev->addr = addr;
    dev->low_speed = (speed == BSP_USB_SPEED_LOW);
    dev->ctrl_mps = 8;
    return dev;
}

void bsp_usb_device_detach(bsp_usb_dev_t* dev) {
    if (dev == NULL || !dev->used) {
        return;
    }
    memset(dev, 0, sizeof(*dev));
}

int bsp_usb_device_update_mps0(bsp_usb_dev_t* dev, uint8_t mps0) {
    if (dev == NULL || !dev->used) {
        return -1;
    }
    dev->ctrl_mps = mps0;
    return 0;
}

/* no TT bookkeeping needed: devices behind an FS hub are reached by
   their own address, FS/LS behind a HS hub is simply impossible here */
int bsp_usb_device_configure_hub(bsp_usb_dev_t* dev, int num_ports) {
    (void)dev;
    (void)num_ports;
    return 0;
}

/* ---- transfers ---- */

int bsp_usb_control_xfer(bsp_usb_dev_t* dev, const usb_setup_pkt_t* setup,
        void* data, bool dir_in) {
    if (dev == NULL || !dev->used) {
        return -1;
    }
    return dwc2_control_xfer(dev->addr, dev->low_speed, dev->ctrl_mps,
            setup, data, dir_in);
}

int bsp_usb_int_in_open(bsp_usb_dev_t* dev, uint8_t ep_addr, uint16_t mps,
        uint8_t interval) {
    uint32_t iv;

    if (dev == NULL || !dev->used) {
        return -1;
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

    if (dev == NULL || !dev->used || dev->int_ep_addr != ep_addr ||
            dev->int_mps == 0) {
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
    if (ret == 0) {
        dev->int_idle_polls++;
        return 0;
    }
    dev->int_idle_polls++;
    return -1;
}

/* generic bulk API stays stubbed: the MSC path below uses dwc2's
   dedicated bulk channels with its own retry/recovery policy */
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

#define MSC_BULK_TIMEOUT_MS 2000u
#define MSC_CBW_TIMEOUT_MS 500u
#define MSC_NAK_RETRIES 3000

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

/* one bulk transaction with the NAK retry loop: flash devices NAK while
   programming, so retry until the phase moves or a hard error aborts */
static int msc_bulk_xfer(bool dir_in, uint8_t ep_addr, uint16_t mps,
        uint8_t* toggle, void* data, uint32_t len, uint32_t timeout_ms) {
    int tries = 0;
    int xret;

    do {
        xret = dwc2_bulk_xfer(dir_in, _msc.dev->addr, _msc.dev->low_speed,
                ep_addr & 0x0Fu, mps, toggle, data, len, timeout_ms);
        if (xret == DWC2_XFER_RETRY && (++tries % 50) == 0) {
            proc_usleep(1000);
        }
    } while (xret == DWC2_XFER_RETRY && tries < MSC_NAK_RETRIES);
    return xret;
}

static void msc_recover(void) {
    dwc2_msc_recover(_msc.dev->addr, _msc.dev->low_speed,
            _msc.dev->ctrl_mps, _msc.iface_num, _msc.ep_in, _msc.ep_out);
    _msc.toggle_in = 0;
    _msc.toggle_out = 0;
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

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_OPCODE_INQUIRY;
    cdb[4] = sizeof(inquiry);
    memset(inquiry, 0, sizeof(inquiry));
    if (usb_msc_command(cdb, 6, true, inquiry, sizeof(inquiry)) != 0) {
        klog("bsp_usb: msc inquiry_failed addr=%u\n", dev->addr);
        memset(&_msc, 0, sizeof(_msc));
        return -1;
    }
    klog("bsp_usb: msc inquiry addr=%u type=%02x vendor=%.8s product=%.16s\n",
            dev->addr, inquiry[0], (const char*)(inquiry + 8),
            (const char*)(inquiry + 16));

    /* media may need a spin-up/debounce window after plug-in */
    {
        int ready = -1;
        for (int i = 0; i < 20; ++i) {
            ready = usb_msc_test_unit_ready();
            if (ready == 0) {
                break;
            }
            proc_usleep(100000);
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
    klog("bsp_usb: msc attached addr=%u sectors=%u size=%u\n",
            dev->addr, _msc.sector_count, _msc.sector_size);

    /* auto-mount the FAT32 volume: spawn a fat32fsd bound to this device.
       A stale mount left by a daemon that did not exit blocks the new
       mount, so skip the spawn in that case instead of double-mounting. */
    {
        fsinfo_t mnt_info;
        if (vfs_get_by_name("/mnt/udisk0", &mnt_info) == 0) {
            klog("bsp_usb: msc mount busy, /mnt/udisk0 still mounted\n");
        }
        else {
            int pid = fork();
            if (pid == 0) {
                proc_detach();
                if (proc_exec("/drivers/fat32fsd -u /dev/hid0 /mnt/udisk0") != 0) {
                    exit(-1);
                }
            }
            else if (pid > 0) {
                _msc.child_pid = pid;
                klog("bsp_usb: msc mounting /mnt/udisk0 pid=%d\n", pid);
            }
            else {
                klog("bsp_usb: msc mount_fork_failed\n");
            }
        }
    }
    return 0;
}

/* scan the config descriptor for a bulk-only mass-storage interface and
   claim it; only the first MSC interface is used */
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
    klog("bsp_usb: msc found addr=%u iface=%u subclass=%u ep_in=%02x ep_out=%02x\n",
            dev->addr, msc_iface->bInterfaceNumber, msc_iface->bInterfaceSubClass,
            ep_in, ep_out);
    return msc_attach(dev, msc_iface->bInterfaceNumber,
            ep_in, ep_out, mps_in, mps_out);
}

/* device (or the tree it sits on) is gone: tell the fs daemon to exit
   and drop all MSC state. Safe to call when nothing is attached. */
void bsp_usb_msc_detach(bsp_usb_dev_t* dev) {
    if (!_msc.claimed || _msc.dev != dev) {
        return;
    }
    klog("bsp_usb: msc detached addr=%u\n", _msc.dev->addr);
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
