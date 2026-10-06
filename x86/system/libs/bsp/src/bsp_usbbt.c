/*
 * bsp_usbbt.c: Intel Bluetooth HCI service for the x86 USB host stack.
 *
 * The Bluetooth function of the Intel combo cards (AX211, AX411, BE202
 * and their CNVio2 successors) is a USB-attached HCI device (VID 0x8087,
 * class/subclass/protocol e0/01/01); iwlwifi drives only the WiFi
 * function over PCIe. The Linux references for this file are
 * drivers/bluetooth/btusb.c (USB transport: HCI commands over EP0
 * control with bmRequestType 0x20, events over interrupt-IN, ACL over
 * bulk, 0xfc09 firmware fragments over bulk while in bootloader mode,
 * and no command-complete for the 0xfc01 Intel Reset) and
 * drivers/bluetooth/btintel.c (TLV version format, ibt-*.sfi firmware
 * selection from the CNVi/CNVr top registers, CSS header handling,
 * secure-send download, bootup / secure-send-result vendor events and
 * the DDC configuration records).
 *
 * The service is claimed at enumeration (bsp_usb_bt_probe, after MSC)
 * and runs the whole firmware setup synchronously inside usbhostd - the
 * only process allowed to touch the xHCI rings. The btd daemon receives
 * the resulting H4 byte stream over dev_cntl (USBHCI_CMD_* protocol,
 * ewoksys/usbhci.h) via the machine's bsp_bt.c client. Only xHCI-backed
 * devices are claimed: the UHCI driver has no bulk endpoint model, and
 * the PCH bluetooth function always hangs off the xHCI root hub.
 */
#include <bsp/bsp_usb.h>
#include <ewoksys/usbhci.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/klog.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

/* H4 packet type bytes (mirrored onto the RX ring for btd) */
#define H4_CMD  0x01
#define H4_ACL  0x02
#define H4_EVT  0x04

/* HCI event codes used during setup */
#define HCI_EVT_CMD_COMPLETE 0x0e
#define HCI_EVT_CMD_STATUS   0x0f
#define HCI_EVT_VENDOR       0xff

/* Intel vendor HCI opcodes (btintel.h) */
#define INTEL_OP_RESET        0xfc01 /* no command complete comes back */
#define INTEL_OP_READ_VERSION 0xfc05
#define INTEL_OP_SECURE_SEND  0xfc09
#define INTEL_OP_WRITE_DDC    0xfc8b
#define INTEL_OP_WRITE_BOOT_PARAMS 0xfc0e /* record inside the .sfi payload */
#define INTEL_OP_DSBR         0xfc0a

/* Intel vendor event subevents (event code 0xff) */
#define INTEL_EVT_BOOTUP            0x02
#define INTEL_EVT_SECURE_SEND_RESULT 0x06

/* TLV fields of the 0xfc05 response (btintel.h enum) */
#define INTEL_TLV_CNVI_TOP   0x10
#define INTEL_TLV_CNVR_TOP   0x11
#define INTEL_TLV_CNVI_BT    0x12
#define INTEL_TLV_IMAGE_TYPE 0x1c
#define INTEL_TLV_TIME_STAMP 0x1d
#define INTEL_TLV_BUILD_TYPE 0x1e
#define INTEL_TLV_BUILD_NUM  0x1f
#define INTEL_TLV_MIN_FW     0x2d
#define INTEL_TLV_LIMITED_CCE 0x2e
#define INTEL_TLV_SBE_TYPE   0x2f
#define INTEL_TLV_OTP_BDADDR 0x30
#define INTEL_TLV_FW_ID      0x50

#define INTEL_IMG_BOOTLOADER 0x01
#define INTEL_IMG_IML        0x02
#define INTEL_IMG_OP         0x03

#define INTEL_HW_PLATFORM(c) (((c) & 0x0000ff00u) >> 8)
#define INTEL_HW_VARIANT(c)  (((c) & 0x003f0000u) >> 16)

/* .sfi layout (btintel.c): RSA CSS header 644 bytes, optional ECDSA
   header 320 bytes, then the HCI command payload; CSS header version
   sits at offset 8 */
#define INTEL_RSA_HEADER_LEN    644u
#define INTEL_ECDSA_OFFSET      644u
#define INTEL_ECDSA_HEADER_LEN  320u
#define INTEL_CSS_VER_OFFSET    8u
#define INTEL_CSS_VER_RSA       0x00010000u
#define INTEL_CSS_VER_ECDSA     0x00020000u
#define INTEL_CSS_VER_HYBRID    0x00069700u
/* hybrid (hw_variant >= 0x20): CSS 128 @0, ECDSA pkey+sig 96+96 @128,
   LMS pkey+sig 52+1744 @320, payload follows */
#define INTEL_HYBRID_CMD_OFFSET 2116u

#define INTEL_FW_DIR "/usr/lib/firmware/intel/"
#define INTEL_FW_MAX (4u * 1024u * 1024u)
#define INTEL_DDC_MAX (64u * 1024u)

/* HCI command via EP0: host-to-device, class, device (btusb cmdreq) */
#define USB_REQTYPE_BT_CMD 0x20

/* RX ring for the H4 byte stream served to btd */
#define BT_RX_RING 8192u
/* one chunk per endpoint poll (FS interrupt/bulk mps is 64) */
#define BT_POLL_BUF 64u
/* assemble buffers: event 2+255, acl 4+1280 (Intel ACL mps is 1024) */
#define BT_EVT_ASM_CAP (2u + 255u)
#define BT_ACL_ASM_CAP (4u + 1280u)

/* setup-time timeouts */
#define BT_CMD_TIMEOUT_MS     1000u
#define BT_FRAGMENT_TIMEOUT_MS 2000u
#define BT_BOOTUP_TIMEOUT_MS  5000u
#define BT_DOWNLOAD_TIMEOUT_MS 5000u

typedef struct {
    uint8_t* buf;
    uint32_t cap;
    uint32_t have;
    uint32_t want;   /* full frame length incl. header, 0 = still in header */
    uint8_t  h4;     /* type byte emitted with complete frames */
} h4_asm_t;

typedef struct {
    uint32_t cnvi_top;
    uint32_t cnvr_top;
    uint32_t cnvi_bt;
    uint8_t  img_type;
    uint8_t  build_type;
    uint8_t  min_fw_nn;
    uint8_t  min_fw_cw;
    uint8_t  min_fw_yy;
    uint8_t  limited_cce;
    uint8_t  sbe_type;
    uint16_t timestamp;
    uint32_t build_num;
    char     fw_id[64];
} intel_ver_t;

typedef struct {
    bool claimed;
    bool ready;      /* operational firmware running, HCI stream usable */
    bool bootloader; /* device sits in bootloader mode (bulk-in carries events) */
    bool busy;       /* setup in progress (guards USBHCI_CMD_SETUP) */
    bsp_usb_dev_t* dev;
    uint16_t vid, pid;
    uint8_t  ep_evt;     /* interrupt IN: HCI events */
    uint8_t  ep_acl_in;  /* bulk IN: ACL data (events too, in bootloader mode) */
    uint8_t  ep_acl_out; /* bulk OUT */
    /* H4 stream served to btd */
    uint8_t  rx[BT_RX_RING];
    uint32_t rx_r, rx_w;
    uint32_t rx_dropped;
    uint8_t  evt_asm_buf[BT_EVT_ASM_CAP];
    uint8_t  acl_asm_buf[BT_ACL_ASM_CAP];
    h4_asm_t evt_asm;
    h4_asm_t acl_asm;
    intel_ver_t ver;
    char fw_name[64]; /* last firmware basename tried (diagnostics) */
    uint32_t setup_stage; /* 0 none, 1 first image, 2 IML second image */
} usb_bt_t;

static usb_bt_t _bt;

static inline uint16_t le16(const void* p) {
    const uint8_t* b = (const uint8_t*)p;
    return (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
}

static inline uint32_t le32(const void* p) {
    const uint8_t* b = (const uint8_t*)p;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
            ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static inline uint16_t swab16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

static inline uint64_t now_ms(void) {
    return kernel_tic_ms(0);
}

/* ---------------- H4 assembly + RX ring ---------------- */

static void h4_asm_init(h4_asm_t* a, uint8_t* buf, uint32_t cap, uint8_t h4) {
    a->buf = buf;
    a->cap = cap;
    a->have = 0;
    a->want = 0;
    a->h4 = h4;
}

static uint32_t rx_free(void) {
    return BT_RX_RING - (_bt.rx_w - _bt.rx_r) - 1u;
}

/* emit one complete H4 frame; drop (counted) when the ring is full -
   HCI flow control makes this the never-taken path, a gap is still
   better than a corruption */
static void rx_emit(const h4_asm_t* a) {
    uint32_t need = 1u + a->want;
    if (rx_free() < need) {
        _bt.rx_dropped++;
        return;
    }
    uint32_t w = _bt.rx_w % BT_RX_RING;
    _bt.rx[w] = a->h4;
    for (uint32_t i = 0; i < a->want; ++i) {
        w = (_bt.rx_w + 1u + i) % BT_RX_RING;
        _bt.rx[w] = a->buf[i];
    }
    _bt.rx_w += need;
}

static void h4_feed(h4_asm_t* a, const uint8_t* data, uint32_t len) {
    for (uint32_t i = 0; i < len; ++i) {
        if (a->have >= a->cap) {
            /* frame larger than the assembler: restart at the next one */
            a->have = 0;
            a->want = 0;
        }
        a->buf[a->have++] = data[i];
        if (a->want == 0) {
            if (a->h4 == H4_EVT && a->have >= 2) {
                a->want = 2u + a->buf[1];
            }
            else if (a->h4 == H4_ACL && a->have >= 4) {
                a->want = 4u + le16(a->buf + 2);
            }
        }
        if (a->want != 0 && a->have == a->want) {
            rx_emit(a);
            a->have = 0;
            a->want = 0;
        }
    }
}

static void rx_reset(void) {
    _bt.rx_r = _bt.rx_w = 0;
    _bt.evt_asm.have = _bt.evt_asm.want = 0;
    _bt.acl_asm.have = _bt.acl_asm.want = 0;
}

/* ---------------- endpoint pumping ---------------- */

/* one non-blocking pass over the IN endpoints; events always come from
   the interrupt pipe, in bootloader mode the bulk pipe carries event
   bytes too (btusb_recv_bulk_intel), afterwards it is pure ACL */
static void bt_pump(void) {
    uint8_t buf[BT_POLL_BUF];

    for (int i = 0; i < 4; ++i) {
        int n = bsp_usb_int_in_poll(_bt.dev, _bt.ep_evt, buf, sizeof(buf));
        if (n > 0) {
            h4_feed(&_bt.evt_asm, buf, (uint32_t)n);
        }
        if (n <= 0) {
            break;
        }
    }
    for (int i = 0; i < 4; ++i) {
        int n = bsp_usb_bulk_in_poll(_bt.dev, _bt.ep_acl_in, buf, sizeof(buf));
        if (n > 0) {
            h4_feed(_bt.bootloader ? &_bt.evt_asm : &_bt.acl_asm,
                    buf, (uint32_t)n);
        }
        if (n <= 0) {
            break;
        }
    }
}

void bsp_usb_bt_poll(void) {
    if (!_bt.claimed) {
        return;
    }
    bt_pump();
}

/* ---------------- setup-time HCI sync engine ----------------
   Strictly request->response, driven from probe; events that do not
   match the current wait are dropped (the stream is quiet during
   setup). Runs on the same endpoints as the runtime pump, so the
   assemblers/ring are reset when setup (re)starts. */

/* pop one assembled event frame from the event assembler's OWN buffer
   (during setup events never reach the RX ring: h4_emit is bypassed by
   draining the assembler directly) */
static bool evt_take(uint8_t* out, uint32_t* out_len) {
    h4_asm_t* a = &_bt.evt_asm;
    if (a->want == 0 || a->have < a->want) {
        return false;
    }
    uint32_t n = a->want;
    if (n > *out_len) {
        n = *out_len;
    }
    memcpy(out, a->buf, n);
    *out_len = n;
    a->have = 0;
    a->want = 0;
    return true;
}

/* setup variant of the pump: events are collected but NOT emitted to
   the ring (h4_feed would); feed bytes into the assembler by hand */
static void setup_pump(void) {
    uint8_t buf[BT_POLL_BUF];
    h4_asm_t* a = &_bt.evt_asm;

    for (int i = 0; i < 4; ++i) {
        int n = bsp_usb_int_in_poll(_bt.dev, _bt.ep_evt, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        for (int k = 0; k < n && a->want == 0; ++k) {
            if (a->have < a->cap) {
                a->buf[a->have++] = buf[k];
                if (a->have >= 2) {
                    a->want = 2u + a->buf[1];
                }
            }
        }
        /* whole transfer beyond a completed frame is dropped: setup is
           strictly one outstanding command, nothing else talks */
    }
    for (int i = 0; i < 4; ++i) {
        int n = bsp_usb_bulk_in_poll(_bt.dev, _bt.ep_acl_in, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        if (!_bt.bootloader) {
            continue; /* ACL has no business arriving during setup */
        }
        for (int k = 0; k < n && a->want == 0; ++k) {
            if (a->have < a->cap) {
                a->buf[a->have++] = buf[k];
                if (a->have >= 2) {
                    a->want = 2u + a->buf[1];
                }
            }
        }
    }
}

/* wait for a command-complete of want_opcode (>0) or a vendor subevent
   (want_opcode == 0); params of the matching event land in out */
static int evt_wait(uint16_t want_opcode, uint8_t want_sub,
        uint8_t* out, uint32_t out_cap, uint32_t* out_len,
        uint32_t timeout_ms) {
    uint8_t frame[BT_EVT_ASM_CAP];
    uint64_t deadline = now_ms() + timeout_ms;

    for (;;) {
        setup_pump();
        uint32_t n = sizeof(frame);
        if (evt_take(frame, &n)) {
            uint8_t code = frame[0];
            uint8_t plen = frame[1];
            const uint8_t* params = frame + 2;
            if (want_opcode != 0 &&
                    code == HCI_EVT_CMD_COMPLETE && plen >= 3 &&
                    le16(params + 1) == want_opcode) {
                if (plen - 3u > out_cap) {
                    plen = (uint8_t)(3u + out_cap);
                }
                *out_len = plen - 3u;
                memcpy(out, params + 3, *out_len);
                return 0;
            }
            if (want_opcode != 0 &&
                    code == HCI_EVT_CMD_STATUS && plen >= 4 &&
                    le16(params + 2) == want_opcode) {
                /* interim status; nonzero is terminal */
                if (params[0] != 0) {
                    return -1;
                }
            }
            else if (want_opcode == 0 &&
                    code == HCI_EVT_VENDOR && plen >= 1 &&
                    params[0] == want_sub) {
                if ((uint32_t)plen > out_cap) {
                    plen = (uint8_t)out_cap;
                }
                *out_len = plen;
                memcpy(out, params, plen);
                return 0;
            }
            /* non-matching events are dropped */
            continue;
        }
        if ((int64_t)(now_ms() - deadline) >= 0) {
            return -1;
        }
        usleep(1000);
    }
}

/* send one HCI command frame [opcode, plen, params]; 0xfc09 firmware
   fragments ride the bulk pipe (btusb_send_frame_intel), everything
   else is an EP0 control transfer with bmRequestType 0x20 */
static int bt_send_cmd_frame(uint16_t opcode, const uint8_t* params,
        uint8_t plen) {
    uint8_t frame[3 + 255];
    frame[0] = (uint8_t)(opcode & 0xffu);
    frame[1] = (uint8_t)(opcode >> 8);
    frame[2] = plen;
    if (plen > 0) {
        memcpy(frame + 3, params, plen);
    }
    uint32_t len = 3u + plen;

    if (opcode == INTEL_OP_SECURE_SEND) {
        return bsp_usb_bulk_xfer(_bt.dev, _bt.ep_acl_out, frame,
                (int)len, false) == (int)len ? 0 : -1;
    }
    usb_setup_pkt_t setup;
    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = USB_REQTYPE_BT_CMD;
    setup.bRequest = 0;
    setup.wValue = 0;
    setup.wIndex = 0;
    setup.wLength = (uint16_t)len;
    return bsp_usb_control_xfer(_bt.dev, &setup, frame, false) >= 0 ? 0 : -1;
}

/* command + command-complete; evt gets the return params (status first) */
static int bt_hci_cmd_sync(uint16_t opcode, const uint8_t* params,
        uint8_t plen, uint8_t* evt, uint32_t evt_cap, uint32_t* evt_len,
        uint32_t timeout_ms) {
    if (bt_send_cmd_frame(opcode, params, plen) != 0) {
        return -1;
    }
    return evt_wait(opcode, 0, evt, evt_cap, evt_len, timeout_ms);
}

/* ---------------- btintel TLV setup flow ---------------- */

static void tlv_parse(const uint8_t* data, uint32_t len, intel_ver_t* ver) {
    memset(ver, 0, sizeof(*ver));
    uint32_t off = 0;
    while (off + 2 <= len) {
        uint8_t type = data[off];
        uint8_t tlen = data[off + 1];
        if (off + 2u + tlen > len) {
            break;
        }
        const uint8_t* val = data + off + 2;
        switch (type) {
        case INTEL_TLV_CNVI_TOP:
            if (tlen >= 4) { ver->cnvi_top = le32(val); }
            break;
        case INTEL_TLV_CNVR_TOP:
            if (tlen >= 4) { ver->cnvr_top = le32(val); }
            break;
        case INTEL_TLV_CNVI_BT:
            if (tlen >= 4) { ver->cnvi_bt = le32(val); }
            break;
        case INTEL_TLV_IMAGE_TYPE:
            if (tlen >= 1) { ver->img_type = val[0]; }
            break;
        case INTEL_TLV_TIME_STAMP:
            if (tlen >= 2) {
                ver->timestamp = le16(val);
                ver->min_fw_cw = val[0];
                ver->min_fw_yy = val[1];
            }
            break;
        case INTEL_TLV_BUILD_TYPE:
            if (tlen >= 1) { ver->build_type = val[0]; }
            break;
        case INTEL_TLV_BUILD_NUM:
            if (tlen >= 4) {
                ver->build_num = le32(val);
                ver->min_fw_nn = val[0];
            }
            break;
        case INTEL_TLV_MIN_FW:
            if (tlen >= 3) {
                ver->min_fw_nn = val[0];
                ver->min_fw_cw = val[1];
                ver->min_fw_yy = val[2];
            }
            break;
        case INTEL_TLV_LIMITED_CCE:
            if (tlen >= 1) { ver->limited_cce = val[0]; }
            break;
        case INTEL_TLV_SBE_TYPE:
            if (tlen >= 1) { ver->sbe_type = val[0]; }
            break;
        case INTEL_TLV_FW_ID: {
            uint32_t n = tlen < sizeof(ver->fw_id) - 1u ?
                    tlen : sizeof(ver->fw_id) - 1u;
            memcpy(ver->fw_id, val, n);
            ver->fw_id[n] = 0;
            break;
        }
        default:
            break;
        }
        off += 2u + tlen;
    }
}

/* 0xfc05 {0xff} -> TLV version block (btintel_read_version_tlv) */
static int intel_read_version(intel_ver_t* ver) {
    uint8_t evt[256];
    uint32_t evt_len = 0;
    uint8_t param = 0xff;

    if (bt_hci_cmd_sync(INTEL_OP_READ_VERSION, &param, 1,
            evt, sizeof(evt), &evt_len, BT_CMD_TIMEOUT_MS) != 0) {
        return -1;
    }
    if (evt_len < 1 || evt[0] != 0) {
        return -1;
    }
    tlv_parse(evt + 1, evt_len - 1, ver);
    return 0;
}

/* secure send: 0xfc09 fragments of <=252 payload bytes, each acked with
   a command complete (btintel_secure_send) */
static int intel_secure_send(uint8_t frag_type, const uint8_t* data,
        uint32_t len) {
    uint8_t evt[16];
    while (len > 0) {
        uint8_t chunk = len > 252u ? 252u : (uint8_t)len;
        uint8_t params[253];
        uint32_t evt_len = 0;

        params[0] = frag_type;
        memcpy(params + 1, data, chunk);
        if (bt_hci_cmd_sync(INTEL_OP_SECURE_SEND, params,
                (uint8_t)(chunk + 1u), evt, sizeof(evt), &evt_len,
                BT_FRAGMENT_TIMEOUT_MS) != 0) {
            return -1;
        }
        if (evt_len < 1 || evt[0] != 0) {
            return -1;
        }
        len -= chunk;
        data += chunk;
    }
    return 0;
}

/* RSA CSS header: init fragment 128B CSS, 256B pkey @128, 256B sig @388
   (btintel_sfi_rsa_header_secure_send) */
static int intel_send_rsa_header(const uint8_t* fw) {
    if (intel_secure_send(0x00, fw, 128) != 0) {
        return -1;
    }
    if (intel_secure_send(0x03, fw + 128, 256) != 0) {
        return -1;
    }
    return intel_secure_send(0x02, fw + 388, 256);
}

/* ECDSA CSS header @644: 128B CSS, 96B pkey, 96B sig
   (btintel_sfi_ecdsa_header_secure_send) */
static int intel_send_ecdsa_header(const uint8_t* fw) {
    if (intel_secure_send(0x00, fw + INTEL_ECDSA_OFFSET, 128) != 0) {
        return -1;
    }
    if (intel_secure_send(0x03, fw + INTEL_ECDSA_OFFSET + 128, 96) != 0) {
        return -1;
    }
    return intel_secure_send(0x02, fw + INTEL_ECDSA_OFFSET + 224, 96);
}

/* hybrid CSS (hw_variant >= 0x20): 128B CSS, ECDSA pkey/sig @128,
   LMS pkey/sig @320 (btintel_sfi_hybrid_header_secure_send) */
static int intel_send_hybrid_header(const uint8_t* fw) {
    if (intel_secure_send(0x00, fw, 128) != 0) {
        return -1;
    }
    if (intel_secure_send(0x03, fw + 128, 96) != 0) {
        return -1;
    }
    if (intel_secure_send(0x02, fw + 128 + 96, 96) != 0) {
        return -1;
    }
    if (intel_secure_send(0x05, fw + 320, 52) != 0) {
        return -1;
    }
    return intel_secure_send(0x04, fw + 320 + 52, 1744);
}

/* payload: raw HCI commands accumulated to a 4-byte boundary and sent
   as one data fragment each (btintel_download_firmware_payload); the
   file carries Intel_NOP padding so the walk always ends aligned */
static int intel_download_payload(const uint8_t* fw, uint32_t fw_len,
        uint32_t offset) {
    uint32_t pos = offset;
    uint32_t frag_len = 0;

    while (pos + frag_len < fw_len) {
        if (pos + frag_len + 3u > fw_len) {
            return -1; /* truncated command header */
        }
        uint8_t plen = fw[pos + frag_len + 2];
        frag_len += 3u + plen;
        if (pos + frag_len > fw_len) {
            return -1;
        }
        if ((frag_len & 3u) == 0) {
            if (intel_secure_send(0x01, fw + pos, frag_len) != 0) {
                return -1;
            }
            pos += frag_len;
            frag_len = 0;
        }
    }
    return frag_len == 0 ? 0 : -1;
}

/* walk the payload for the 0xfc0e record: it carries the per-SKU boot
   address for the Intel Reset and the file's firmware build, compared
   against the running build to skip a redundant download
   (btintel_firmware_version). Returns 1 when the file matches the
   running firmware (already loaded). */
static int intel_fw_probe(const uint8_t* fw, uint32_t fw_len,
        uint32_t offset, const intel_ver_t* ver, uint32_t* boot_param) {
    uint32_t pos = offset;

    while (pos + 3u <= fw_len) {
        uint16_t opcode = le16(fw + pos);
        uint8_t plen = fw[pos + 2];
        if (pos + 3u + plen > fw_len) {
            break;
        }
        if (opcode == INTEL_OP_WRITE_BOOT_PARAMS && plen >= 7) {
            *boot_param = le32(fw + pos + 3);
            /* params: {le32 boot_addr, u8 build_num, u8 build_ww,
               u8 build_yy, u8 build_nn} (struct btintel_boot_params) */
            return (ver->min_fw_nn == fw[pos + 7] &&
                    ver->min_fw_cw == fw[pos + 8] &&
                    ver->min_fw_yy == fw[pos + 9]) ? 1 : 0;
        }
        pos += 3u + plen;
    }
    return 0;
}

/* one image: header fragments + payload + wait for the secure-send
   result vendor event (btintel_download_fw_tlv +
   btintel_download_wait). Returns 1 when the running firmware already
   matches (download skipped, boot still required), 0 downloaded, <0
   error. */
static int intel_download_image(const intel_ver_t* ver, const uint8_t* fw,
        uint32_t fw_len, uint32_t* boot_param) {
    uint8_t hw = INTEL_HW_VARIANT(ver->cnvi_bt);
    uint32_t css_ver;
    uint32_t payload_off;
    uint8_t evt[16];
    uint32_t evt_len = 0;

    if (fw_len < INTEL_RSA_HEADER_LEN + 16u) {
        klog("bsp_usb: bt fw too small %u bytes\n", fw_len);
        return -1;
    }
    css_ver = le32(fw + INTEL_CSS_VER_OFFSET);

    /* header selection by hardware variant / secure boot engine
       (btintel_download_fw_tlv) */
    if (css_ver != INTEL_CSS_VER_RSA && css_ver != INTEL_CSS_VER_HYBRID) {
        klog("bsp_usb: bt fw bad css ver 0x%x\n", css_ver);
        return -1;
    }
    if (hw >= 0x17 && css_ver == INTEL_CSS_VER_RSA) {
        if (fw_len < INTEL_ECDSA_OFFSET + INTEL_ECDSA_HEADER_LEN ||
                fw[INTEL_ECDSA_OFFSET] != 0x06 ||
                le32(fw + INTEL_ECDSA_OFFSET + INTEL_CSS_VER_OFFSET) !=
                INTEL_CSS_VER_ECDSA) {
            klog("bsp_usb: bt fw bad ecdsa header\n");
            return -1;
        }
        payload_off = INTEL_ECDSA_OFFSET + INTEL_ECDSA_HEADER_LEN;
    }
    else if (hw >= 0x20 && css_ver == INTEL_CSS_VER_HYBRID) {
        if (fw_len < INTEL_HYBRID_CMD_OFFSET) {
            return -1;
        }
        payload_off = INTEL_HYBRID_CMD_OFFSET;
    }
    else {
        payload_off = INTEL_RSA_HEADER_LEN;
    }

        int probe = intel_fw_probe(fw, fw_len, payload_off, ver, boot_param);
    if (probe > 0) {
        return 1;
    }

    if (hw >= 0x20 && css_ver == INTEL_CSS_VER_HYBRID) {
        if (intel_send_hybrid_header(fw) != 0) {
            return -1;
        }
    }
    else if (hw >= 0x17 && ver->sbe_type == 0x01) {
        if (intel_send_ecdsa_header(fw) != 0) {
            return -1;
        }
    }
    else {
        if (intel_send_rsa_header(fw) != 0) {
            return -1;
        }
    }

    if (intel_download_payload(fw, fw_len, payload_off) != 0) {
        klog("bsp_usb: bt fw payload send failed\n");
        return -1;
    }

    /* the bootloader reports the download result with a vendor event */
    if (evt_wait(0, INTEL_EVT_SECURE_SEND_RESULT,
            evt, sizeof(evt), &evt_len, BT_DOWNLOAD_TIMEOUT_MS) != 0) {
        klog("bsp_usb: bt fw download result timeout\n");
        return -1;
    }
    if (evt_len < 4 || evt[1] != 0) {
        klog("bsp_usb: bt fw download rejected result=%u\n",
                evt_len >= 2 ? evt[1] : 0xff);
        return -1;
    }
    return 0;
}

/* Intel Reset + bootup vendor event (btintel_boot): 0xfc01 gets no
   command complete, the operational firmware announces itself */
static int intel_boot(uint32_t boot_param) {
    uint8_t params[8];
    uint8_t evt[16];
    uint32_t evt_len = 0;

    /* soft reset, patch enabled, boot from the downloaded image */
    params[0] = 0x00;
    params[1] = 0x01;
    params[2] = 0x00;
    params[3] = 0x01;
    params[4] = (uint8_t)(boot_param & 0xffu);
    params[5] = (uint8_t)(boot_param >> 8);
    params[6] = (uint8_t)(boot_param >> 16);
    params[7] = (uint8_t)(boot_param >> 24);

    if (bt_send_cmd_frame(INTEL_OP_RESET, params, sizeof(params)) != 0) {
        return -1;
    }
    if (evt_wait(0, INTEL_EVT_BOOTUP, evt, sizeof(evt), &evt_len,
            BT_BOOTUP_TIMEOUT_MS) != 0) {
        klog("bsp_usb: bt bootup event timeout\n");
        return -1;
    }
    return 0;
}

/* firmware file name from the CNVi/CNVr top registers
   (btintel_get_fw_name_tlv): ibt-<cnvi>-<cnvr>[-iml|-fwid].<suffix> */
static void intel_fw_name(const intel_ver_t* ver, const char* suffix,
        char* out, size_t out_sz) {
    uint16_t cnvi = swab16((uint16_t)(((ver->cnvi_top & 0xfffu) << 4) |
            ((ver->cnvi_top >> 24) & 0xfu)));
    uint16_t cnvr = swab16((uint16_t)(((ver->cnvr_top & 0xfffu) << 4) |
            ((ver->cnvr_top >> 24) & 0xfu)));

    if (INTEL_HW_VARIANT(ver->cnvi_bt) >= 0x1e) {
        /* Blazar and newer: the bootloader loads the intermediate
           loader image first */
        if (ver->img_type == INTEL_IMG_BOOTLOADER) {
            snprintf(out, out_sz, "ibt-%04x-%04x-iml.%s", cnvi, cnvr, suffix);
            return;
        }
        if (ver->fw_id[0] != 0) {
            snprintf(out, out_sz, "ibt-%04x-%04x-%s.%s",
                    cnvi, cnvr, ver->fw_id, suffix);
            return;
        }
    }
    snprintf(out, out_sz, "ibt-%04x-%04x.%s", cnvi, cnvr, suffix);
}

static uint8_t* bt_load_file(const char* name, uint32_t cap, uint32_t* out_len) {
    char path[128];
    snprintf(path, sizeof(path), "%s%s", INTEL_FW_DIR, name);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return NULL;
    }
    uint8_t* buf = (uint8_t*)malloc(cap);
    if (buf == NULL) {
        close(fd);
        return NULL;
    }
    uint32_t len = 0;
    while (len < cap) {
        int r = read(fd, buf + len, cap - len);
        if (r <= 0) {
            break;
        }
        len += (uint32_t)r;
    }
    close(fd);
    if (len == 0) {
        free(buf);
        return NULL;
    }
    *out_len = len;
    return buf;
}

/* DDC records [len][id 2B][value...] over 0xfc8b, best effort
   (btintel_load_ddc_config) */
static void intel_load_ddc(const intel_ver_t* ver) {
    char name[80];
    uint32_t fw_len = 0;
    uint8_t evt[8];

    intel_fw_name(ver, "ddc", name, sizeof(name));
    uint8_t* fw = bt_load_file(name, INTEL_DDC_MAX, &fw_len);
    if (fw == NULL) {
        return; /* optional: the device works without it */
    }
    uint32_t pos = 0;
    while (pos < fw_len) {
        uint32_t plen = fw[pos] + 1u;
        if (plen < 3u || plen > fw_len - pos) {
            break;
        }
        uint32_t evt_len = 0;
        if (bt_hci_cmd_sync(INTEL_OP_WRITE_DDC, fw + pos, (uint8_t)plen,
                evt, sizeof(evt), &evt_len, BT_CMD_TIMEOUT_MS) != 0) {
            klog("bsp_usb: bt ddc write failed off=%u\n", pos);
            break;
        }
        pos += plen;
    }
    free(fw);
}

/* DSBR (BRI response drive strength): required on Gale Peak / BlazarU
   in operational mode over USB (btintel_set_dsbr). The value comes from
   a UEFI variable Linux reads; with no UEFI in the picture the zeros of
   its efi-read-failure path are sent. */
static void intel_set_dsbr(const intel_ver_t* ver) {
    uint8_t hw = INTEL_HW_VARIANT(ver->cnvi_bt);
    if (hw != 0x1c && hw != 0x1d) {
        return;
    }
    uint8_t evt[8];
    uint32_t evt_len = 0;
    uint8_t params[2] = { 0x00, 0x00 };
    (void)bt_hci_cmd_sync(INTEL_OP_DSBR, params, sizeof(params),
            evt, sizeof(evt), &evt_len, BT_CMD_TIMEOUT_MS);
}

/* the whole bring-up, mirroring btintel_setup_combined +
   btintel_bootloader_setup_tlv for TLV devices (hw_variant >= 0x17,
   which covers AX211/AX411/BE202 and successors) */
static int intel_setup(void) {
    _bt.evt_asm.have = _bt.evt_asm.want = 0;
    _bt.acl_asm.have = _bt.acl_asm.want = 0;
    _bt.rx_r = _bt.rx_w = 0;
    _bt.bootloader = true;

    if (intel_read_version(&_bt.ver) != 0) {
        klog("bsp_usb: bt read version failed vid=%04x pid=%04x\n",
                _bt.vid, _bt.pid);
        return -1;
    }
    intel_ver_t* ver = &_bt.ver;

    if (INTEL_HW_PLATFORM(ver->cnvi_bt) != 0x37) {
        klog("bsp_usb: bt unsupported platform 0x%x\n",
                INTEL_HW_PLATFORM(ver->cnvi_bt));
        return -1;
    }
    uint8_t hw = INTEL_HW_VARIANT(ver->cnvi_bt);
    if (hw < 0x17 || hw > 0x22) {
        klog("bsp_usb: bt unsupported hw variant 0x%x\n", hw);
        return -1;
    }
    if (ver->img_type == INTEL_IMG_OP) {
        /* operational firmware already running (warm reboot) */
        _bt.bootloader = false;
        return 0;
    }
    if (ver->limited_cce != 0) {
        klog("bsp_usb: bt limited cce unsupported\n");
        return -1;
    }

    /* bootloader -> (IML) -> operational: Blazar boots through an
       intermediate loader image, everything else reaches OP in one
       pass; the loop just re-reads the version and iterates while the
       device is not operational yet */
    for (_bt.setup_stage = 1; _bt.setup_stage <= 2; ++_bt.setup_stage) {
        uint32_t boot_param = 0;
        uint32_t fw_len = 0;

        intel_fw_name(ver, "sfi", _bt.fw_name, sizeof(_bt.fw_name));
        uint8_t* fw = bt_load_file(_bt.fw_name, INTEL_FW_MAX, &fw_len);
        if (fw == NULL) {
            klog("bsp_usb: bt firmware %s%s missing\n",
                    INTEL_FW_DIR, _bt.fw_name);
            return -1;
        }
        int dret = intel_download_image(ver, fw, fw_len, &boot_param);
        free(fw);
        if (dret < 0) {
            return -1;
        }
        if (intel_boot(boot_param) != 0) {
            return -1;
        }
        if (intel_read_version(ver) != 0) {
            return -1;
        }
        if (ver->img_type == INTEL_IMG_OP) {
            break;
        }
        if (ver->img_type != INTEL_IMG_IML) {
            klog("bsp_usb: bt unexpected img=%u after boot\n", ver->img_type);
            return -1;
        }
        /* IML running: second pass downloads the operational image */
    }

    if (ver->img_type != INTEL_IMG_OP) {
        return -1;
    }
    _bt.bootloader = false;

    intel_set_dsbr(ver);
    intel_load_ddc(ver);

    return 0;
}

/* ---------------- bsp hooks ---------------- */

int bsp_usb_bt_probe(bsp_usb_dev_t* dev, uint16_t vid, uint16_t pid,
        const uint8_t* cfg, int cfg_len) {
    if (_bt.claimed || dev == NULL || cfg == NULL) {
        return -1;
    }
    if (vid != 0x8087 || !bsp_usb_xhci_owned(dev)) {
        return -1;
    }

    /* first e0/01/01 interface: interrupt-IN (events) + bulk IN/OUT */
    bool in_bt_iface = false;
    uint8_t ep_evt = 0, ep_in = 0, ep_out = 0, interval = 1;
    uint16_t mps_evt = 0, mps_in = 0, mps_out = 0;

    for (int off = 0; off + 2 <= cfg_len; ) {
        uint8_t len = cfg[off];
        uint8_t type = cfg[off + 1];
        if (len < 2 || off + len > cfg_len) {
            break;
        }
        if (type == USB_DESC_INTERFACE && len >= (int)sizeof(usb_iface_desc_t)) {
            const usb_iface_desc_t* iface =
                    (const usb_iface_desc_t*)(cfg + off);
            if (!in_bt_iface && iface->bInterfaceClass == 0xe0 &&
                    iface->bInterfaceSubClass == 0x01 &&
                    iface->bInterfaceProtocol == 0x01) {
                in_bt_iface = true;
            }
            else if (in_bt_iface) {
                break; /* endpoints of the next interface are not ours */
            }
        }
        else if (type == USB_DESC_ENDPOINT && in_bt_iface &&
                len >= (int)sizeof(usb_ep_desc_t)) {
            const usb_ep_desc_t* ep = (const usb_ep_desc_t*)(cfg + off);
            uint8_t xtype = ep->bmAttributes & 0x3u;
            bool dir_in = (ep->bEndpointAddress & USB_ENDPOINT_IN) != 0;
            if (xtype == USB_ENDPOINT_XFER_INTERRUPT && dir_in && ep_evt == 0) {
                ep_evt = ep->bEndpointAddress;
                mps_evt = (uint16_t)(ep->wMaxPacketSize & 0x07ffu);
                interval = ep->bInterval;
            }
            else if (xtype == USB_ENDPOINT_XFER_BULK) {
                if (dir_in && ep_in == 0) {
                    ep_in = ep->bEndpointAddress;
                    mps_in = (uint16_t)(ep->wMaxPacketSize & 0x07ffu);
                }
                else if (!dir_in && ep_out == 0) {
                    ep_out = ep->bEndpointAddress;
                    mps_out = (uint16_t)(ep->wMaxPacketSize & 0x07ffu);
                }
            }
        }
        off += len;
    }

    if (ep_evt == 0 || ep_in == 0 || ep_out == 0) {
        return -1;
    }

    memset(&_bt.ver, 0, sizeof(_bt.ver));
    _bt.dev = dev;
    _bt.vid = vid;
    _bt.pid = pid;
    _bt.ep_evt = ep_evt;
    _bt.ep_acl_in = ep_in;
    _bt.ep_acl_out = ep_out;
    h4_asm_init(&_bt.evt_asm, _bt.evt_asm_buf, sizeof(_bt.evt_asm_buf), H4_EVT);
    h4_asm_init(&_bt.acl_asm, _bt.acl_asm_buf, sizeof(_bt.acl_asm_buf), H4_ACL);
    rx_reset();
    _bt.rx_dropped = 0;
    _bt.fw_name[0] = 0;
    _bt.setup_stage = 0;

    if (bsp_usb_int_in_open(dev, ep_evt, mps_evt, interval) != 0 ||
            bsp_usb_bulk_open(dev, ep_in, mps_in) != 0 ||
            bsp_usb_bulk_open(dev, ep_out, mps_out) != 0) {
        klog("bsp_usb: bt endpoint open failed\n");
        return -1;
    }

    _bt.claimed = true;
    _bt.ready = false;
    _bt.busy = true;

    if (intel_setup() == 0) {
        _bt.ready = true;
    }
    _bt.busy = false;
    return 0;
}

void bsp_usb_bt_detach(bsp_usb_dev_t* dev) {
    if (!_bt.claimed || _bt.dev != dev) {
        return;
    }
    _bt.claimed = false;
    _bt.ready = false;
    _bt.dev = NULL;
}

bool bsp_usb_bt_attached(bsp_usb_dev_t* dev) {
    return _bt.claimed && _bt.dev == dev;
}

/* bsp_usb_reinit invalidates every device handle wholesale; drop the
   claim without touching the now-stale pointer (the assembler state is
   re-armed by the next probe) */
void bsp_usb_bt_reset(void) {
    if (_bt.claimed) {
        klog("bsp_usb: bt dropped on usb reinit\n");
    }
    memset(&_bt, 0, sizeof(_bt));
}

/* USBHCI_CMD_* served on the usbhostd device node for btd */
int bsp_usb_bt_cntl(vdevice_t* vdev, int from_pid, int cmd,
        proto_t* in, proto_t* out) {
    (void)vdev;
    (void)from_pid;

    switch (cmd) {
    case USBHCI_CMD_INFO:
        PF->addi(out, _bt.claimed ? 1 : 0);
        PF->addi(out, _bt.ready ? 1 : 0);
        PF->addi(out, _bt.vid);
        PF->addi(out, _bt.pid);
        return 0;
    case USBHCI_CMD_SETUP: {
        /* btd retry: re-run the firmware setup (firmware file may have
           appeared since the probe); a no-op while already up */
        if (!_bt.claimed || _bt.busy) {
            return -1;
        }
        if (_bt.ready) {
            return 0;
        }
        _bt.busy = true;
        if (intel_setup() == 0) {
            _bt.ready = true;
        }
        _bt.busy = false;
        return _bt.ready ? 0 : -1;
    }
    case USBHCI_CMD_SEND: {
        if (!_bt.ready) {
            return -1;
        }
        int pkt_type = proto_read_int(in);
        int32_t sz = 0;
        void* data = proto_read(in, &sz);
        if (data == NULL || sz <= 0 || sz > 4096) {
            return -1;
        }
        if (pkt_type == H4_CMD) {
            /* full HCI command frame -> EP0 (same shape the setup
               engine sends) */
            usb_setup_pkt_t setup;
            memset(&setup, 0, sizeof(setup));
            setup.bmRequestType = USB_REQTYPE_BT_CMD;
            setup.bRequest = 0;
            setup.wValue = 0;
            setup.wIndex = 0;
            setup.wLength = (uint16_t)sz;
            return bsp_usb_control_xfer(_bt.dev, &setup, data, false) >= 0
                    ? 0 : -1;
        }
        if (pkt_type == H4_ACL) {
            return bsp_usb_bulk_xfer(_bt.dev, _bt.ep_acl_out, data,
                    sz, false) == sz ? 0 : -1;
        }
        return -1;
    }
    case USBHCI_CMD_RECV: {
        int max = proto_read_int(in);
        if (max <= 0) {
            return -1;
        }
        uint32_t avail = _bt.rx_w - _bt.rx_r;
        uint32_t n = avail < (uint32_t)max ? avail : (uint32_t)max;
        uint8_t buf[512];
        while (n > 0) {
            uint32_t step = n > sizeof(buf) ? (uint32_t)sizeof(buf) : n;
            for (uint32_t i = 0; i < step; ++i) {
                buf[i] = _bt.rx[(_bt.rx_r + i) % BT_RX_RING];
            }
            PF->add(out, buf, step);
            _bt.rx_r += step;
            n -= step;
        }
        return 0;
    }
    case USBHCI_CMD_FLUSH:
        rx_reset();
        return 0;
    case USBHCI_CMD_DIAG: {
        char s[96];
        snprintf(s, sizeof(s), "claimed=%d ready=%d rx=%u drop=%u",
                _bt.claimed ? 1 : 0, _bt.ready ? 1 : 0,
                _bt.rx_w - _bt.rx_r, _bt.rx_dropped);
        PF->add(out, s, strlen(s) + 1);
        return 0;
    }
    default:
        return -1;
    }
}
