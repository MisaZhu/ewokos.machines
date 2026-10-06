/* bsp_bt.c: x86 bluetooth HCI transport behind the bsp_bt contract.
 *
 * The Intel combo-card bluetooth function (AX211/AX411/BE202, VID 0x8087,
 * class e0/01/01) is a USB device claimed by usbhostd at enumeration; the
 * whole btusb/btintel firmware setup runs there (bsp_usbbt.c) and the H4
 * byte stream is served over dev_cntl on usbhostd's device node with the
 * USBHCI_CMD_* protocol (ewoksys/usbhci.h). This file is the client side:
 * a buffered shim that turns btd's byte-at-a-time recv polling into one
 * IPC round-trip per chunk, exactly like the usbfat32fsd MSC client.
 *
 * No patchram image is reported to btd (bsp_bt_firmware returns NULL):
 * the .sfi download already happened inside usbhostd before this shim
 * reports the transport up.
 */
#include <bt/bsp_bt.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/usbhci.h>
#include <ewoksys/kernel_tic.h>
#include <string.h>
#include <unistd.h>

/* usbhostd's device node (the same /dev/hid0 the MSC client uses) */
#define BT_USB_NODE "/dev/hid0"
/* bytes pulled per USBHCI_CMD_RECV round-trip */
#define BT_RECV_CHUNK 512

static int _usb_pid = -1;
static uint8_t _rx[BT_RECV_CHUNK];
static int _rx_len = 0; /* valid bytes in _rx */
static int _rx_pos = 0; /* next unread byte */

static int usb_pid(void) {
    if (_usb_pid < 0) {
        _usb_pid = dev_get_pid(BT_USB_NODE);
    }
    return _usb_pid;
}

/* one USBHCI_CMD_* round-trip; returns the dev_cntl status */
static int bt_cntl(int cmd, proto_t* in, proto_t* out) {
    int pid = usb_pid();
    if (pid < 0) {
        return -1;
    }
    return dev_cntl_by_pid(pid, cmd, in, out);
}

int bsp_bt_init(bool recovery) {
    proto_t in, out;

    if (recovery) {
        /* usbhostd may have been restarted; resolve again */
        _usb_pid = -1;
    }
    if (usb_pid() < 0) {
        return -1;
    }

    /* ask the host side to (re)run the firmware setup when the adapter
       was claimed but is not operational yet; a no-op while up */
    PF->init(&in);
    PF->init(&out);
    (void)bt_cntl(USBHCI_CMD_SETUP, &in, &out);
    PF->clear(&in);
    PF->clear(&out);

    PF->init(&in);
    PF->init(&out);
    if (bt_cntl(USBHCI_CMD_INFO, &in, &out) != 0) {
        PF->clear(&in);
        PF->clear(&out);
        return -1;
    }
    int claimed = proto_read_int(&out);
    int ready = proto_read_int(&out);
    PF->clear(&in);
    PF->clear(&out);
    if (!claimed || !ready) {
        return -1;
    }

    _rx_len = _rx_pos = 0;
    (void)bsp_bt_flush();
    return 0;
}

void bsp_bt_power_off(void) {
    /* the adapter stays claimed by usbhostd; only drop the local state so
       the next init re-runs the setup handshake */
    _rx_len = _rx_pos = 0;
}

int bsp_bt_send(uint8_t pkt_type, const uint8_t* data, size_t len) {
    proto_t in, out;
    int ret;

    if (data == NULL || len == 0) {
        return -1;
    }
    PF->init(&in)->addi(&in, pkt_type);
    PF->add(&in, (void*)data, (uint32_t)len);
    PF->init(&out);
    ret = bt_cntl(USBHCI_CMD_SEND, &in, &out);
    PF->clear(&in);
    PF->clear(&out);
    return ret;
}

/* refill the local buffer from the host-side H4 ring */
static int bt_refill(void) {
    proto_t in, out;

    PF->init(&in)->addi(&in, BT_RECV_CHUNK);
    PF->init(&out);
    if (bt_cntl(USBHCI_CMD_RECV, &in, &out) != 0) {
        PF->clear(&in);
        PF->clear(&out);
        _rx_len = _rx_pos = 0;
        _usb_pid = -1; /* host may be gone; re-resolve next time */
        return -1;
    }
    PF->clear(&in);

    int32_t sz = 0;
    void* data = proto_read(&out, &sz);
    if (data != NULL && sz > 0) {
        if (sz > BT_RECV_CHUNK) {
            sz = BT_RECV_CHUNK;
        }
        memcpy(_rx, data, (uint32_t)sz);
        _rx_len = sz;
        _rx_pos = 0;
    }
    PF->clear(&out);
    return _rx_len > 0 ? 0 : -1;
}

int bsp_bt_recv(uint32_t timeout_ms) {
    uint64_t deadline = kernel_tic_ms(0) + timeout_ms;

    for (;;) {
        if (_rx_pos < _rx_len) {
            return _rx[_rx_pos++];
        }
        if (bt_refill() == 0) {
            continue;
        }
        if (timeout_ms == 0 || kernel_tic_ms(0) >= deadline) {
            return -1;
        }
        usleep(1000);
    }
}

int bsp_bt_flush(void) {
    proto_t in, out;
    int dropped = _rx_len - _rx_pos;

    _rx_len = _rx_pos = 0;
    PF->init(&in);
    PF->init(&out);
    (void)bt_cntl(USBHCI_CMD_FLUSH, &in, &out);
    PF->clear(&in);
    PF->clear(&out);
    return dropped;
}

void bsp_bt_firmware(const uint8_t** data, uint32_t* len) {
    /* the Intel .sfi setup is usbhostd's job (bsp_usbbt.c), btd's
       Broadcom patchram path stays unused on this machine */
    *data = NULL;
    *len = 0;
}

void bsp_bt_diag_str(char* buf, size_t size) {
    proto_t in, out;

    if (size == 0) {
        return;
    }
    buf[0] = 0;
    PF->init(&in);
    PF->init(&out);
    if (bt_cntl(USBHCI_CMD_DIAG, &in, &out) == 0) {
        int32_t sz = 0;
        const char* s = (const char*)proto_read(&out, &sz);
        if (s != NULL && sz > 0) {
            strncpy(buf, s, size - 1);
            buf[size - 1] = 0;
        }
    }
    PF->clear(&in);
    PF->clear(&out);
}
