#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

#include <ewoksys/vdevice.h>
#include <ewoksys/vfs.h>
#include <ewoksys/charbuf.h>
#include <ewoksys/mmio.h>
#include <ewoksys/proc.h>
#include <ewoksys/ipc.h>
#include <ewoksys/kernel_tic.h>

#include <arch/bcm2712/mmio.h>

#include "firmware_4345c0.h"

#define MAX_BT_DEVICES 32
#define MAX_BT_KNOWN 32
#define MAX_HCI_PAYLOAD 260
#define MAX_EVT_LINE 256
#define BT_CMD_RET_SZ 2048
#define BT_UART_PKT_FOLLOW_TIMEOUT_MS 120

/* persistent store of every device we connected (or paired) with, so a
   future adapter power-on can page them again without a manual scan */
#define BT_KNOWN_DIR "/etc/bt"
#define BT_KNOWN_FILE "/etc/bt/bt.json"
#define BT_KNOWN_MAX_FILE 16384

#define HCI_PKT_COMMAND 0x01
#define HCI_PKT_ACL 0x02
#define HCI_PKT_EVENT 0x04

#define HCI_OGF_LINK_CTRL 0x01
#define HCI_OGF_HOST_CTRL 0x03
#define HCI_OGF_INFO 0x04
#define HCI_OGF_VENDOR 0x3f

#define HCI_OCF_INQUIRY 0x0001
#define HCI_OCF_INQUIRY_CANCEL 0x0002
#define HCI_OCF_CREATE_CONN 0x0005
#define HCI_OCF_DISCONNECT 0x0006
#define HCI_OCF_ACCEPT_CONN_REQ 0x0009
#define HCI_OCF_REJECT_CONN_REQ 0x000a
#define HCI_OCF_LINK_KEY_REQ_REPLY 0x000b
#define HCI_OCF_LINK_KEY_REQ_NEG_REPLY 0x000c
#define HCI_OCF_PIN_CODE_REQ_REPLY 0x000d
#define HCI_OCF_PIN_CODE_REQ_NEG_REPLY 0x000e
#define HCI_OCF_AUTH_REQ 0x0011
#define HCI_OCF_REMOTE_NAME_REQ 0x0019
#define HCI_OCF_IO_CAPABILITY_REQ_REPLY 0x002b
#define HCI_OCF_USER_CONFIRM_REQ_REPLY 0x002c
#define HCI_OCF_USER_CONFIRM_REQ_NEG_REPLY 0x002d
#define HCI_OCF_USER_PASSKEY_REQ_REPLY 0x002e
#define HCI_OCF_USER_PASSKEY_REQ_NEG_REPLY 0x002f

#define HCI_OCF_SET_EVENT_MASK 0x0001
#define HCI_OCF_RESET 0x0003
#define HCI_OCF_READ_BUFFER_SIZE 0x0005
#define HCI_OCF_READ_LOCAL_VERSION 0x0001
#define HCI_OCF_WRITE_SCAN_ENABLE 0x001a
#define HCI_OCF_WRITE_AUTH_ENABLE 0x0020
#define HCI_OCF_WRITE_INQUIRY_MODE 0x0045
#define HCI_OCF_WRITE_SIMPLE_PAIRING_MODE 0x0056

#define HCI_OCF_VENDOR_RESET_CHIP 0x0003
#define HCI_OCF_VENDOR_LOAD_FIRMWARE 0x002e

#define HCI_OPCODE(ogf, ocf) (uint16_t)((((ogf) & 0x3f) << 10) | ((ocf) & 0x03ff))

#define EVT_INQUIRY_COMPLETE 0x01
#define EVT_INQUIRY_RESULT 0x02
#define EVT_CONN_COMPLETE 0x03
#define EVT_CONN_REQUEST 0x04
#define EVT_DISCONN_COMPLETE 0x05
#define EVT_AUTH_COMPLETE 0x06
#define EVT_REMOTE_NAME_COMPLETE 0x07
#define EVT_CMD_COMPLETE 0x0e
#define EVT_CMD_STATUS 0x0f
#define EVT_HARDWARE_ERROR 0x10
#define EVT_PIN_CODE_REQUEST 0x16
#define EVT_LINK_KEY_REQUEST 0x17
#define EVT_LINK_KEY_NOTIFY 0x18
#define EVT_INQUIRY_RESULT_RSSI 0x22
#define EVT_IO_CAPABILITY_REQUEST 0x31
#define EVT_USER_CONFIRMATION_REQUEST 0x33
#define EVT_USER_PASSKEY_REQUEST 0x34
#define EVT_SIMPLE_PAIRING_COMPLETE 0x36
#define EVT_EXTENDED_INQUIRY_RESULT 0x2f
#define EVT_NUM_COMPLETED_PKTS 0x13

/*
 * Standard Bluetooth mouse (HID over Bluetooth): after the ACL link is up
 * and authenticated, the host opens two L2CAP channels - HID Control on
 * PSM 0x0011 and HID Interrupt on PSM 0x0013 - sends SET_PROTOCOL(boot)
 * on the control channel and receives boot mouse reports
 * ([buttons, dx, dy, (wheel)]) as HIDP DATA|INPUT on the interrupt one.
 * A reconnecting mouse opens both channels itself, so the L2CAP layer
 * accepts inbound connection requests for the HID PSMs as well.
 */
#define L2CAP_CID_SIGNAL 0x0001

#define L2CAP_SIG_CMD_REJECT 0x01
#define L2CAP_SIG_CONN_REQ 0x02
#define L2CAP_SIG_CONN_RSP 0x03
#define L2CAP_SIG_CONF_REQ 0x04
#define L2CAP_SIG_CONF_RSP 0x05
#define L2CAP_SIG_DISCONN_REQ 0x06
#define L2CAP_SIG_DISCONN_RSP 0x07
#define L2CAP_SIG_ECHO_REQ 0x08
#define L2CAP_SIG_ECHO_RSP 0x09

#define L2CAP_CONN_SUCCESS 0x0000
#define L2CAP_CONN_PSM_UNSUPPORTED 0x0002
#define L2CAP_CONF_SUCCESS 0x0000

#define L2CAP_PSM_HID_CTRL 0x0011
#define L2CAP_PSM_HID_INTR 0x0013

#define L2CAP_MTU_DEFAULT 672

/* l2cap_chan_t.state */
#define L2CAP_STATE_CLOSED 0
#define L2CAP_STATE_CONN_REQ_SENT 1
#define L2CAP_STATE_CONF_SENT 2
#define L2CAP_STATE_OPEN 3
#define L2CAP_STATE_CLOSING 4

#define L2CAP_STEP_TIMEOUT_MS 2000
#define L2CAP_STEP_MAX_RETRIES 3

/* HIDP transaction headers (high nibble = message type) */
#define HIDP_TRANS_HANDSHAKE 0x00
#define HIDP_TRANS_HID_CONTROL 0x10
#define HIDP_TRANS_SET_PROTOCOL 0x70
#define HIDP_TRANS_DATA 0xA0

#define HIDP_HANDSHAKE_SUCCESS 0x00
#define HIDP_HID_CONTROL_VC_UNPLUG 0x15
#define HIDP_DATA_INPUT 0xA1 /* DATA | input report */

#define HIDP_PROTOCOL_BOOT 0x00

/* subscriber fan-out (same wire protocol as usbhostd's /dev/hid0, see
   libs/usb/usb_defs.h): fcntl cmd 0 selects the report id, then read()
   pops fixed 7-byte pointer events from the per-fd queue */
#define BT_REPORT_ID_MOUSE 1
#define BT_SUBSCRIBER_EVENT_SIZE 7
#define BT_SUBSCRIBER_QUEUE_DEPTH 32
#define BT_SUBSCRIBER_MAX 4
#define BT_HID_REASSERT_MS 30

/*
 * Pi 5 (BCM2712) Bluetooth hardware, from the official device tree
 * (bcm2712.dtsi + bcm2712-rpi-5-b.dts):
 *   - the HCI UART is the SoC 16550 ("brcm,bcm7271-uart", reg 0x20 bytes,
 *     32-bit registers at stride 4) "uarta" @ 0x7d50c000 (main window
 *     offset 0x0150c000), 96MHz uartclk, on gpio24(BT_RTS)/25(BT_CTS)/
 *     26(BT_TXD)/27(BT_RXD) - pinctrl function "uart0", fsel 4 on all
 *     four pins on the D0 stepping - with hardware flow control
 *     (uart-has-rtscts / auto-flow-control). NOTE: uarta is NOT a PL011;
 *     only the console uart10 is.
 *   - BT_ON is "gio" brcmstb-gpio bank0 GPIO29 active high (the dts
 *     bt_shutdown_pins); WL_ON is GPIO28, shared with the wlan driver.
 *   - no external 32kHz clock: the Pi4 GPCLK2 scheme does not exist
 *     here, the chip LPO runs from its own crystal.
 */
#define PI5_UARTA_OFF 0x0150C000u
/* 16550/bcm7271 register map, byte offsets from the uart base */
#define UARTA_THR_REG ((uintptr_t)_mmio_base + PI5_UARTA_OFF + 0x00u)
#define UARTA_RBR_REG ((uintptr_t)_mmio_base + PI5_UARTA_OFF + 0x00u)
#define UARTA_DLL_REG ((uintptr_t)_mmio_base + PI5_UARTA_OFF + 0x00u)
#define UARTA_IER_REG ((uintptr_t)_mmio_base + PI5_UARTA_OFF + 0x04u)
#define UARTA_DLM_REG ((uintptr_t)_mmio_base + PI5_UARTA_OFF + 0x04u)
#define UARTA_FCR_REG ((uintptr_t)_mmio_base + PI5_UARTA_OFF + 0x08u)
#define UARTA_LCR_REG ((uintptr_t)_mmio_base + PI5_UARTA_OFF + 0x0cu)
#define UARTA_MCR_REG ((uintptr_t)_mmio_base + PI5_UARTA_OFF + 0x10u)
#define UARTA_LSR_REG ((uintptr_t)_mmio_base + PI5_UARTA_OFF + 0x14u)
#define UARTA_MSR_REG ((uintptr_t)_mmio_base + PI5_UARTA_OFF + 0x18u)

#define UART_LSR_DR   (1u << 0) /* rx data ready */
#define UART_LSR_THRE (1u << 5) /* tx holding register empty */

#define PI5_BT_UART_CLOCK_HZ 96000000u
#define PI5_BT_BAUD_RATE 115200u

/* The gpio24-27 fsel fields (4 bits each, shifts 0/4/8/12) all live in
   pinctrl register 0x08; fsel 4 is the uart0 function that routes them
   to uarta. The gpio28/gpio29 fields (shifts 16/20) of the same register
   must stay at function 0 so the gio bank keeps driving WL_ON/BT_ON. */
#define PI5_PINCTRL_FSEL_REG  0x08u
#define PI5_BT_UART_FSEL_MASK 0x0000FFFFu
#define PI5_BT_UART_FSEL_VAL  0x00004444u
#define PI5_WL_ON_FSEL_MASK   (0xfu << 16)
#define PI5_BT_ON_FSEL_MASK   (0xfu << 20)

#define PI5_GIO_BASE ((uintptr_t)_mmio_base + PI5_GIO_OFF)
#define GIO_ODEN  0x00u
#define GIO_DATA  0x04u
#define GIO_IODIR 0x08u
#define WL_ON_BIT (1u << 28)
#define BT_ON_BIT (1u << 29)

typedef struct {
    bool used;
    uint8_t addr[6];
    uint32_t class_of_device;
    uint16_t clock_offset;
    uint8_t page_scan_rep_mode;
    int8_t rssi;
    uint16_t handle;
    bool connected;
    bool has_link_key;
    uint8_t link_key[16];
    char name[64];
} bt_device_t;

typedef enum {
    BT_PENDING_NONE = 0,
    BT_PENDING_CONNECT,
    BT_PENDING_PAIR
} bt_pending_type_t;

typedef struct {
    bt_pending_type_t type;
    uint8_t addr[6];
    char pin[17];
    uint16_t handle;
} bt_pending_t;

typedef struct {
    bool active;
    bool done;
    uint16_t opcode;
    int status;
    /* command-complete return parameters (everything after the status
       byte), captured for the synchronous commands that need them */
    bool got_ret;
    uint8_t ret[64];
    uint8_t ret_len;
} bt_wait_cmd_t;

typedef struct {
    uint32_t packets_seen;
    uint32_t event_packets;
    uint32_t acl_packets;
    uint32_t other_packets;
    uint8_t last_pkt_type;
    uint8_t last_event_code;
    uint8_t last_event_len;
    uint16_t last_opcode;
    int last_status;
} bt_wait_debug_t;

typedef struct {
    bool used;
    uint8_t addr[6];
    bool paired;
    bool has_key;
    uint8_t key[16];
    char name[64];
} bt_known_t;

/* one L2CAP connection-oriented channel (HID control or interrupt) */
typedef struct {
    bool used;
    uint16_t acl_handle;
    uint16_t psm;
    uint16_t local_cid;   /* our dynamic source cid (0x0040+) */
    uint16_t remote_cid;  /* 0 until the peer tells us */
    uint8_t state;        /* L2CAP_STATE_* */
    bool conf_req_sent;
    bool conf_rsp_recv;
    bool conf_req_recv;   /* peer asked us to configure its side */
    bool conf_rsp_sent;
    uint8_t sig_id;       /* identifier of the outstanding request */
    uint8_t retries;
    uint64_t retry_ms;
} l2cap_chan_t;

#define MAX_L2CAP_CHANS 4

/* HID host state for one ACL link (control + interrupt channel pair) */
typedef struct {
    bool active;
    uint16_t acl_handle;
    uint8_t addr[6];
    l2cap_chan_t* ctrl;
    l2cap_chan_t* intr;
    bool boot_protocol_ok; /* SET_PROTOCOL(boot) handshake succeeded */
    bool up;               /* both channels open: reports flow */
} bt_hid_chan_t;

/* one /dev/bt0 subscriber fd (mirrors usbhidsrv's fd table) */
typedef struct {
    bool used;
    int fd;
    int from_pid;
    uint8_t report_id; /* 0 = text event stream, BT_REPORT_ID_MOUSE = hid */
    uint8_t q_rd;
    uint8_t q_wr;
    uint8_t q_len[BT_SUBSCRIBER_QUEUE_DEPTH];
    uint8_t q_data[BT_SUBSCRIBER_QUEUE_DEPTH][8];
} bt_subscriber_t;

static vdevice_t* _bt_dev = NULL;
static charbuf_t* _evt_buf = NULL;
static uint32_t _idle_sleep_us = 1000;
static bool _ready = false;
static bool _powered = false;
static bool _scanning = false;
static bt_device_t _devices[MAX_BT_DEVICES];
static bt_known_t _known[MAX_BT_KNOWN];
static bt_pending_t _pending;
static bt_wait_cmd_t _wait_cmd;
static bt_wait_debug_t _wait_debug;

/* host->controller ACL flow control: credits start at the controller's
   total ACL buffer count (READ_BUFFER_SIZE) and are returned by
   Number_Of_Completed_Packets events */
static uint16_t _acl_credits = 0;

/* single-connection reassembly state: HID PDUs are tiny and only one
   link carries them, so one buffer for the whole daemon is enough */
static uint16_t _reas_handle = 0;
static bool _reas_active = false;
static uint16_t _reas_total = 0;
static uint16_t _reas_pos = 0;
static uint8_t _reas_buf[280];

static l2cap_chan_t _l2chans[MAX_L2CAP_CHANS];
static uint16_t _l2_next_cid = 0x0040;
static uint8_t _l2_next_sig_id = 1;
static bt_hid_chan_t _hid;
static bt_subscriber_t _subs[BT_SUBSCRIBER_MAX];
static uint64_t _sub_reassert_ms = 0;

static void l2cap_step(void);
static void l2cap_chan_close(l2cap_chan_t* ch, bool send_req);
static void bt_hid_link_closed(uint16_t handle, const char* reason);
static void bt_hid_dispatch_mouse(const uint8_t* evt);
static void bt_hid_start(uint16_t handle, const uint8_t* addr);
static int bt_wait_for_opcode(uint16_t opcode, uint32_t timeout_ms);
static int bt_hci_command_sync_ret(uint16_t ogf, uint16_t ocf,
        const uint8_t* params, uint8_t param_len, uint32_t timeout_ms,
        uint8_t* ret_params, uint8_t ret_cap, uint8_t* ret_len);

static uint8_t hci_opcode_lo(uint16_t opcode) {
    return (uint8_t)(opcode & 0xff);
}

static uint8_t hci_opcode_hi(uint16_t opcode) {
    return (uint8_t)((opcode >> 8) & 0xff);
}

static bool bt_addr_equal(const uint8_t* a, const uint8_t* b) {
    return memcmp(a, b, 6) == 0;
}

static void bt_addr_to_str(const uint8_t* addr, char* out, size_t size) {
    snprintf(out, size, "%02X:%02X:%02X:%02X:%02X:%02X",
        addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}

static bool bt_parse_addr(const char* str, uint8_t* addr) {
    unsigned int b[6];

    if (sscanf(str, "%2x:%2x:%2x:%2x:%2x:%2x",
            &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }

    addr[0] = (uint8_t)b[5];
    addr[1] = (uint8_t)b[4];
    addr[2] = (uint8_t)b[3];
    addr[3] = (uint8_t)b[2];
    addr[4] = (uint8_t)b[1];
    addr[5] = (uint8_t)b[0];
    return true;
}

static int bt_pending_matches_addr(const uint8_t* addr) {
    return _pending.type != BT_PENDING_NONE && bt_addr_equal(_pending.addr, addr);
}

static void bt_clear_pending(void) {
    memset(&_pending, 0, sizeof(_pending));
}

static bt_device_t* bt_find_device(const uint8_t* addr, bool create) {
    int i;
    bt_device_t* free_slot = NULL;

    for (i = 0; i < MAX_BT_DEVICES; ++i) {
        if (_devices[i].used) {
            if (bt_addr_equal(_devices[i].addr, addr)) {
                return &_devices[i];
            }
        }
        else if (free_slot == NULL) {
            free_slot = &_devices[i];
        }
    }

    if (!create || free_slot == NULL) {
        return NULL;
    }

    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->used = true;
    memcpy(free_slot->addr, addr, 6);
    free_slot->rssi = 127;
    return free_slot;
}

static bt_device_t* bt_find_device_by_handle(uint16_t handle) {
    int i;

    for (i = 0; i < MAX_BT_DEVICES; ++i) {
        if (_devices[i].used && _devices[i].connected && _devices[i].handle == handle) {
            return &_devices[i];
        }
    }
    return NULL;
}

static void bt_trim_name(char* name) {
    int len;

    if (name == NULL) {
        return;
    }

    len = (int)strlen(name);
    while (len > 0) {
        unsigned char ch = (unsigned char)name[len - 1];
        if (ch == '\0' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            name[len - 1] = 0;
            --len;
            continue;
        }
        break;
    }
}

/* ---- persistent known-device store (/etc/bt/bt.json) -----------------
   small json doc, one device object per line:
   {"addr":"AA:BB:CC:DD:EE:FF","name":"...","paired":1,"key":"32hex"}
   parsed tolerantly (hand edits survive): fields are looked up between
   each "addr" occurrence and the closing '}' of its object. */

static bt_known_t* bt_known_find(const uint8_t* addr) {
    int i;

    for (i = 0; i < MAX_BT_KNOWN; ++i) {
        if (_known[i].used && bt_addr_equal(_known[i].addr, addr)) {
            return &_known[i];
        }
    }
    return NULL;
}

static bt_known_t* bt_known_upsert(const uint8_t* addr) {
    int i;
    bt_known_t* k = bt_known_find(addr);

    if (k != NULL) {
        return k;
    }
    for (i = 0; i < MAX_BT_KNOWN; ++i) {
        if (!_known[i].used) {
            memset(&_known[i], 0, sizeof(_known[i]));
            _known[i].used = true;
            memcpy(_known[i].addr, addr, 6);
            return &_known[i];
        }
    }
    return NULL;
}

static bool bt_json_str_field(const char* begin, const char* end,
        const char* key, char* out, size_t out_sz) {
    char pat[16];
    const char* p;
    size_t n = 0;

    if (out_sz == 0) {
        return false;
    }
    out[0] = 0;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(begin, pat);
    if (p == NULL || p >= end) {
        return false;
    }
    p += strlen(pat);
    while (p < end && (*p == ' ' || *p == ':')) {
        ++p;
    }
    if (p >= end || *p != '"') {
        return false;
    }
    ++p;
    while (p < end && *p != 0 && n + 1 < out_sz) {
        if (*p == '\\' && (p + 1) < end && (p[1] == '"' || p[1] == '\\')) {
            out[n++] = p[1];
            p += 2;
            continue;
        }
        if (*p == '"') {
            break;
        }
        out[n++] = *p++;
    }
    out[n] = 0;
    return true;
}

static bool bt_json_int_field(const char* begin, const char* end,
        const char* key, int* out) {
    char pat[16];
    const char* p;

    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(begin, pat);
    if (p == NULL || p >= end) {
        return false;
    }
    p += strlen(pat);
    while (p < end && (*p == ' ' || *p == ':')) {
        ++p;
    }
    if (p >= end || !isdigit((unsigned char)*p)) {
        return false;
    }
    *out = atoi(p);
    return true;
}

static void bt_key_to_hex(const uint8_t* key, char* out) {
    static const char hexd[] = "0123456789abcdef";
    int i;

    for (i = 0; i < 16; ++i) {
        out[i * 2] = hexd[key[i] >> 4];
        out[i * 2 + 1] = hexd[key[i] & 0x0f];
    }
    out[32] = 0;
}

static bool bt_hex_to_key(const char* hex, uint8_t* key) {
    int i;

    if (strlen(hex) != 32) {
        return false;
    }
    for (i = 0; i < 32; ++i) {
        char c = hex[i];
        int v;
        if (c >= '0' && c <= '9') {
            v = c - '0';
        }
        else if (c >= 'a' && c <= 'f') {
            v = c - 'a' + 10;
        }
        else if (c >= 'A' && c <= 'F') {
            v = c - 'A' + 10;
        }
        else {
            return false;
        }
        if ((i & 1) == 0) {
            key[i / 2] = (uint8_t)(v << 4);
        }
        else {
            key[i / 2] |= (uint8_t)v;
        }
    }
    return true;
}

static void bt_known_sanitize_name(char* name) {
    size_t i;

    for (i = 0; name[i] != 0; ++i) {
        if (!isprint((unsigned char)name[i])) {
            name[i] = '.';
        }
    }
}

static int bt_known_load(void) {
    char* buf;
    char* p;
    int fd;
    int n;
    int count = 0;

    fd = open(BT_KNOWN_FILE, O_RDONLY);
    if (fd < 0) {
        return 0; /* no store yet, not an error */
    }
    buf = (char*)malloc(BT_KNOWN_MAX_FILE + 1);
    if (buf == NULL) {
        close(fd);
        return 0;
    }
    n = (int)read(fd, buf, BT_KNOWN_MAX_FILE);
    close(fd);
    if (n <= 0) {
        free(buf);
        return 0;
    }
    buf[n] = 0;

    p = buf;
    while (count < MAX_BT_KNOWN && (p = strstr(p, "\"addr\"")) != NULL) {
        char* obj_end = strchr(p, '}');
        char addr_str[24];
        char hex[40];
        uint8_t addr[6];
        bt_known_t* k;
        int paired = 0;

        if (obj_end == NULL) {
            break;
        }
        if (bt_json_str_field(p, obj_end, "addr", addr_str, sizeof(addr_str)) &&
                bt_parse_addr(addr_str, addr) &&
                (k = bt_known_upsert(addr)) != NULL) {
            bt_json_str_field(p, obj_end, "name", k->name, sizeof(k->name));
            bt_known_sanitize_name(k->name);
            bt_trim_name(k->name);
            if (bt_json_int_field(p, obj_end, "paired", &paired) && paired) {
                k->paired = true;
            }
            if (bt_json_str_field(p, obj_end, "key", hex, sizeof(hex)) &&
                    bt_hex_to_key(hex, k->key)) {
                k->has_key = true;
                k->paired = true;
            }
            ++count;
        }
        p = obj_end + 1;
    }
    free(buf);
    if (count > 0) {
        slog("bluetooth store loaded devices=%d\n", count);
    }
    return count;
}

static void bt_json_write_escaped(int fd, const char* str) {
    const char* p;

    for (p = str; *p != 0; ++p) {
        if (*p == '"' || *p == '\\') {
            char esc[2];
            esc[0] = '\\';
            esc[1] = *p;
            write(fd, esc, 2);
        }
        else {
            write(fd, p, 1);
        }
    }
}

static int bt_known_save(void) {
    char line[160];
    char hex[40];
    int fd;
    int i;
    int written = 0;

    if (mkdir(BT_KNOWN_DIR, 0755) != 0 && errno != EEXIST) {
        slog("bluetooth store mkdir_failed dir=%s errno=%d\n", BT_KNOWN_DIR, errno);
    }
    fd = open(BT_KNOWN_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        slog("bluetooth store save_failed path=%s errno=%d\n", BT_KNOWN_FILE, errno);
        return -1;
    }
    write(fd, "{\n  \"devices\": [\n", 17);
    for (i = 0; i < MAX_BT_KNOWN; ++i) {
        char addr[24];
        int len;

        if (!_known[i].used) {
            continue;
        }
        bt_addr_to_str(_known[i].addr, addr, sizeof(addr));
        if (_known[i].has_key) {
            bt_key_to_hex(_known[i].key, hex);
        }
        else {
            hex[0] = 0;
        }
        len = snprintf(line, sizeof(line),
            "%s    {\"addr\":\"%s\",\"name\":\"", written > 0 ? ",\n" : "", addr);
        write(fd, line, len);
        bt_json_write_escaped(fd, _known[i].name);
        len = snprintf(line, sizeof(line), "\",\"paired\":%d,\"key\":\"%s\"}\n",
            _known[i].paired ? 1 : 0, hex);
        write(fd, line, len);
        ++written;
    }
    write(fd, "  ]\n}\n", 6);
    close(fd);
    slog("bluetooth store saved devices=%d\n", written);
    return 0;
}

/* record a device we just connected/paired with (link key included when
   we have one, so the next power-on reconnects without re-pairing) */
static void bt_known_touch_from_device(const bt_device_t* dev) {
    bt_known_t* k = bt_known_upsert(dev->addr);

    if (k == NULL) {
        return;
    }
    if (dev->name[0] != 0) {
        strncpy(k->name, dev->name, sizeof(k->name) - 1);
        k->name[sizeof(k->name) - 1] = 0;
    }
    if (dev->has_link_key) {
        k->paired = true;
        k->has_key = true;
        memcpy(k->key, dev->link_key, 16);
    }
    bt_known_save();
}

/* push stored link keys/names into the runtime cache so the controller's
   LINK_KEY_REQUEST can be answered even before any scan ran */
static void bt_known_seed_devices(void) {
    int i;

    for (i = 0; i < MAX_BT_KNOWN; ++i) {
        bt_device_t* dev;

        if (!_known[i].used) {
            continue;
        }
        dev = bt_find_device(_known[i].addr, true);
        if (dev == NULL) {
            continue;
        }
        if (_known[i].name[0] != 0 && dev->name[0] == 0) {
            strncpy(dev->name, _known[i].name, sizeof(dev->name) - 1);
        }
        if (_known[i].has_key) {
            memcpy(dev->link_key, _known[i].key, 16);
            dev->has_link_key = true;
        }
        if (dev->page_scan_rep_mode == 0) {
            dev->page_scan_rep_mode = 1; /* R1, the common case */
        }
    }
}

static void bt_parse_eir_name(const uint8_t* eir, size_t len, char* out, size_t out_sz) {
    size_t pos = 0;

    if (out_sz == 0) {
        return;
    }
    out[0] = 0;

    while (pos < len) {
        uint8_t field_len = eir[pos];
        uint8_t field_type;
        size_t copy_len;
        size_t i;

        if (field_len == 0) {
            break;
        }
        if ((pos + 1 + field_len) > len) {
            break;
        }

        field_type = eir[pos + 1];
        if (field_type == 0x08 || field_type == 0x09) {
            copy_len = field_len - 1;
            if (copy_len >= out_sz) {
                copy_len = out_sz - 1;
            }
            for (i = 0; i < copy_len; ++i) {
                unsigned char ch = eir[pos + 2 + i];
                out[i] = isprint(ch) ? (char)ch : '.';
            }
            out[copy_len] = 0;
            bt_trim_name(out);
            return;
        }
        pos += (size_t)field_len + 1;
    }
}

static void bt_wakeup_readers(void) {
    if (_bt_dev != NULL && _bt_dev->mnt_info.node != 0) {
        vfs_wakeup(_bt_dev->mnt_info.node, VFS_EVT_RD);
    }
}

static void bt_emit(const char* fmt, ...) {
    va_list ap;
    char line[MAX_EVT_LINE];
    int len;
    int i;

    if (_evt_buf == NULL) {
        return;
    }

    va_start(ap, fmt);
    len = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (len < 0) {
        return;
    }
    if (len >= (int)sizeof(line)) {
        len = (int)sizeof(line) - 1;
    }

    ipc_disable();
    for (i = 0; i < len; ++i) {
        charbuf_push(_evt_buf, line[i], true);
    }
    charbuf_push(_evt_buf, '\n', true);
    ipc_enable();
    bt_wakeup_readers();
}

static size_t bt_ret_len(const char* ret, size_t ret_sz) {
    size_t n = 0;

    while (n < ret_sz && ret[n] != 0) {
        ++n;
    }
    return n;
}

static void bt_ret_append(char* ret, size_t ret_sz, const char* fmt, ...) {
    size_t used;
    va_list ap;

    if (ret == NULL || ret_sz == 0) {
        return;
    }

    used = bt_ret_len(ret, ret_sz);
    if (used >= (ret_sz - 1)) {
        return;
    }

    va_start(ap, fmt);
    vsnprintf(ret + used, ret_sz - used, fmt, ap);
    va_end(ap);
}

/* push-pull gio output, same register sequence the wlan platform glue
   uses for WL_REG_ON: open-drain off, direction out, then drive the line */
static void pi5_gio_output(uint32_t bit, bool on) {
    put32(PI5_GIO_BASE + GIO_ODEN, get32(PI5_GIO_BASE + GIO_ODEN) & ~bit);
    put32(PI5_GIO_BASE + GIO_IODIR, get32(PI5_GIO_BASE + GIO_IODIR) & ~bit);
    if (on) {
        put32(PI5_GIO_BASE + GIO_DATA, get32(PI5_GIO_BASE + GIO_DATA) | bit);
    }
    else {
        put32(PI5_GIO_BASE + GIO_DATA, get32(PI5_GIO_BASE + GIO_DATA) & ~bit);
    }
}

static void pi5_bt_uart_init(void) {
    uintptr_t fsel_reg = (uintptr_t)_mmio_base + PI5_PINCTRL_OFF + PI5_PINCTRL_FSEL_REG;
    /* 16550 divisor = uartclk / (16 * baud); 96MHz -> 115200 gives 52
       (+0.16% error, same as linux 8250_bcm7271) */
    uint32_t div = PI5_BT_UART_CLOCK_HZ / (16u * PI5_BT_BAUD_RATE);

    /* mux gpio24-27 onto uarta; leave gio28/29 as plain gpio */
    put32(fsel_reg, (get32(fsel_reg)
            & ~(PI5_BT_UART_FSEL_MASK | PI5_WL_ON_FSEL_MASK | PI5_BT_ON_FSEL_MASK))
        | PI5_BT_UART_FSEL_VAL);

    put32(UARTA_IER_REG, 0x00u); /* polled mode, no irqs */
    put32(UARTA_LCR_REG, 0x80u); /* DLAB: divisor latch access */
    put32(UARTA_DLL_REG, div & 0xffu);
    put32(UARTA_DLM_REG, (div >> 8) & 0xffu);
    put32(UARTA_LCR_REG, 0x03u); /* 8N1 */
    put32(UARTA_FCR_REG, 0x07u); /* fifo enable + rx/tx fifo reset */
    put32(UARTA_FCR_REG, 0x01u); /* keep fifo enabled */
    /* DTR | RTS | AFE: hardware auto RTS/CTS flow control
       (dts: uart-has-rtscts + auto-flow-control) */
    put32(UARTA_MCR_REG, (1u << 0) | (1u << 1) | (1u << 5));
}

static void bt_prepare_combo_chip_power(void) {
    if (access("/dev/wl0", F_OK) == 0) {
        slog("bluetooth init wl0_present keep_wl_on\n");
        return;
    }

    /* gio28 is WL_ON; keep it enabled when BT is started standalone. */
    slog("bluetooth init standalone enable_wl_on\n");
    pi5_gio_output(WL_ON_BIT, true);
    usleep(100000);
}

static void bt_release_bt_shutdown(void) {
    /* gio29 is BT_ON/shutdown-gpios of the Pi 5 combo chip. */
    slog("bluetooth init assert_bt_on\n");
    pi5_gio_output(BT_ON_BIT, false);
    usleep(100000);
    slog("bluetooth init deassert_bt_on\n");
    pi5_gio_output(BT_ON_BIT, true);
    usleep(100000);
}

static int32_t pi5_bt_uart_send(uint8_t c) {
    /* bounded: a healthy uarta drains its fifo in ~1.4ms at 115200; 100ms
       per byte only trips when the chip stops asserting CTS (or the line
       is dead) - without this a stuck flow control hangs the whole daemon
       inside the firmware download */
    uint32_t waited = 0;
    while (!(get32(UARTA_LSR_REG) & UART_LSR_THRE)) {
        if (++waited > 100) {
            slog("bluetooth tx_timeout lsr=0x%02x msr=0x%02x\n",
                get32(UARTA_LSR_REG) & 0xffu, get32(UARTA_MSR_REG) & 0xffu);
            return -1;
        }
        usleep(1000);
    }
    put32(UARTA_THR_REG, c);
    return 0;
}

static int pi5_bt_uart_recv(uint32_t timeout_ms) {
    uint32_t waited = 0;

    while (!(get32(UARTA_LSR_REG) & UART_LSR_DR)) {
        if (waited >= timeout_ms) {
            return -1;
        }
        usleep(1000);
        waited++;
    }
    return (int)(get32(UARTA_RBR_REG) & 0xFFu);
}

static int bt_uart_recv_timeout(uint32_t timeout_ms) {
    return pi5_bt_uart_recv(timeout_ms);
}

static int bt_uart_flush(void) {
    int n = 0;

    /* bounded too: if the uart block ever reads garbage, an endless stream
       of fake "bytes" must not hang init */
    while (n < 1024 && bt_uart_recv_timeout(1) >= 0) {
        ++n;
    }
    return n;
}

static int bt_hci_send_packet(uint8_t pkt_type, const uint8_t* data, size_t len) {
    size_t i;

    if (pi5_bt_uart_send(pkt_type) != 0) {
        return -1;
    }
    for (i = 0; i < len; ++i) {
        if (pi5_bt_uart_send(data[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

static int bt_hci_send_command_raw(uint16_t opcode, const uint8_t* params, uint8_t param_len) {
    uint8_t hdr[3];
    uint8_t i;

    hdr[0] = hci_opcode_lo(opcode);
    hdr[1] = hci_opcode_hi(opcode);
    hdr[2] = param_len;
    if (bt_hci_send_packet(HCI_PKT_COMMAND, hdr, sizeof(hdr)) != 0) {
        return -1;
    }
    if (param_len > 0 && params != NULL) {
        for (i = 0; i < param_len; ++i) {
            if (pi5_bt_uart_send(params[i]) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int bt_hci_send_command(uint16_t ogf, uint16_t ocf, const uint8_t* params, uint8_t param_len) {
    return bt_hci_send_command_raw(HCI_OPCODE(ogf, ocf), params, param_len);
}

static void bt_update_wait_cmd_complete(uint16_t opcode, int status) {
    if (_wait_cmd.active && _wait_cmd.opcode == opcode) {
        _wait_cmd.status = status;
        _wait_cmd.done = true;
    }
}

static void bt_emit_device_line(const char* prefix, const bt_device_t* dev) {
    char addr[24];

    bt_addr_to_str(dev->addr, addr, sizeof(addr));
    bt_emit("%s %s class=0x%06X rssi=%d connected=%d paired=%d name=%s\n",
        prefix,
        addr,
        dev->class_of_device & 0xffffffu,
        (int)dev->rssi,
        dev->connected ? 1 : 0,
        dev->has_link_key ? 1 : 0,
        dev->name[0] ? dev->name : "-");
}

static void bt_ret_append_device_line(int dev_id, char* ret, size_t ret_sz,
        const char* prefix, const bt_device_t* dev) {
    char addr[24];

    bt_addr_to_str(dev->addr, addr, sizeof(addr));
    bt_ret_append(ret, ret_sz,
        "%d: %s %s class=0x%06X rssi=%d connected=%d paired=%d name=%s\n",
        dev_id,
        prefix,
        addr,
        dev->class_of_device & 0xffffffu,
        (int)dev->rssi,
        dev->connected ? 1 : 0,
        dev->has_link_key ? 1 : 0,
        dev->name[0] ? dev->name : "-");
}

static int bt_hci_accept_connection(const uint8_t* addr) {
    uint8_t params[7];

    memcpy(params, addr, 6);
    /* role 0x01: remain slave - the reconnecting mouse initiated the
       link, and a role switch is just extra LMP round trips for a
       battery peripheral */
    params[6] = 0x01;
    return bt_hci_send_command(HCI_OGF_LINK_CTRL, HCI_OCF_ACCEPT_CONN_REQ, params, sizeof(params));
}

static int bt_hci_auth_request(uint16_t handle) {
    uint8_t params[2];

    params[0] = (uint8_t)(handle & 0xff);
    params[1] = (uint8_t)(handle >> 8);
    return bt_hci_send_command(HCI_OGF_LINK_CTRL, HCI_OCF_AUTH_REQ, params, sizeof(params));
}

static int bt_hci_disconnect(uint16_t handle) {
    uint8_t params[3];

    params[0] = (uint8_t)(handle & 0xff);
    params[1] = (uint8_t)(handle >> 8);
    params[2] = 0x13;
    return bt_hci_send_command(HCI_OGF_LINK_CTRL, HCI_OCF_DISCONNECT, params, sizeof(params));
}

static int bt_hci_create_connection(const bt_device_t* dev) {
    uint8_t params[13];
    uint16_t packet_type = 0xcc18;
    uint16_t clock_offset = dev != NULL ? dev->clock_offset : 0;
    uint8_t page_scan_rep_mode = dev != NULL ? dev->page_scan_rep_mode : 0x01;

    memset(params, 0, sizeof(params));
    memcpy(params, dev->addr, 6);
    params[6] = (uint8_t)(packet_type & 0xff);
    params[7] = (uint8_t)(packet_type >> 8);
    params[8] = page_scan_rep_mode;
    params[9] = 0;
    if (clock_offset != 0) {
        clock_offset |= 0x8000;
    }
    params[10] = (uint8_t)(clock_offset & 0xff);
    params[11] = (uint8_t)(clock_offset >> 8);
    params[12] = 1;
    return bt_hci_send_command(HCI_OGF_LINK_CTRL, HCI_OCF_CREATE_CONN, params, sizeof(params));
}

static int bt_hci_request_remote_name(const bt_device_t* dev) {
    uint8_t params[10];

    memset(params, 0, sizeof(params));
    memcpy(params, dev->addr, 6);
    params[6] = dev->page_scan_rep_mode;
    params[7] = 0;
    params[8] = (uint8_t)(dev->clock_offset & 0xff);
    params[9] = (uint8_t)(dev->clock_offset >> 8);
    return bt_hci_send_command(HCI_OGF_LINK_CTRL, HCI_OCF_REMOTE_NAME_REQ, params, sizeof(params));
}

static void bt_handle_inquiry_result_common(
    const uint8_t* addr,
    uint8_t page_scan_rep_mode,
    uint32_t class_of_device,
    uint16_t clock_offset,
    int8_t rssi,
    const char* name) {
    bt_device_t* dev = bt_find_device(addr, true);

    if (dev == NULL) {
        return;
    }

    dev->page_scan_rep_mode = page_scan_rep_mode;
    dev->class_of_device = class_of_device;
    dev->clock_offset = clock_offset;
    dev->rssi = rssi;
    if (name != NULL && name[0] != 0) {
        strncpy(dev->name, name, sizeof(dev->name) - 1);
        dev->name[sizeof(dev->name) - 1] = 0;
        bt_trim_name(dev->name);
    }
    bt_emit_device_line("device", dev);
}

static void bt_handle_command_complete(const uint8_t* payload, size_t len) {
    uint16_t opcode;
    int status = 0;

    if (len < 3) {
        return;
    }

    opcode = (uint16_t)payload[1] | ((uint16_t)payload[2] << 8);
    if (len >= 4) {
        status = payload[3];
    }
    /* capture the return parameters (everything after the status byte) so
       synchronous commands like READ_BUFFER_SIZE can read them back */
    if (_wait_cmd.active && _wait_cmd.opcode == opcode) {
        if (len > 4) {
            size_t n = len - 4;
            if (n > sizeof(_wait_cmd.ret)) {
                n = sizeof(_wait_cmd.ret);
            }
            memcpy(_wait_cmd.ret, payload + 4, n);
            _wait_cmd.ret_len = (uint8_t)n;
            _wait_cmd.got_ret = true;
        }
        else {
            _wait_cmd.ret_len = 0;
            _wait_cmd.got_ret = false;
        }
    }
    _wait_debug.last_opcode = opcode;
    _wait_debug.last_status = status;
    bt_update_wait_cmd_complete(opcode, status);
}

static void bt_handle_command_status(const uint8_t* payload, size_t len) {
    uint16_t opcode;
    int status;

    if (len < 4) {
        return;
    }

    status = payload[0];
    opcode = (uint16_t)payload[2] | ((uint16_t)payload[3] << 8);
    _wait_debug.last_opcode = opcode;
    _wait_debug.last_status = status;
    bt_update_wait_cmd_complete(opcode, status);
}

static void bt_handle_inquiry_result(const uint8_t* payload, size_t len) {
    uint8_t num;
    size_t i;
    size_t off_addr;
    size_t off_psrm;
    size_t off_cod;
    size_t off_clk;

    if (len < 1) {
        return;
    }

    num = payload[0];
    off_addr = 1;
    off_psrm = off_addr + (size_t)num * 6;
    off_cod = off_psrm + (size_t)num * 2;
    off_clk = off_cod + (size_t)num * 3;

    if (len < off_clk + (size_t)num * 2) {
        return;
    }

    for (i = 0; i < num; ++i) {
        const uint8_t* addr = payload + off_addr + i * 6;
        uint8_t psrm = payload[off_psrm + i];
        uint32_t cod = (uint32_t)payload[off_cod + i * 3] |
            ((uint32_t)payload[off_cod + i * 3 + 1] << 8) |
            ((uint32_t)payload[off_cod + i * 3 + 2] << 16);
        uint16_t clk = (uint16_t)payload[off_clk + i * 2] |
            ((uint16_t)payload[off_clk + i * 2 + 1] << 8);

        bt_handle_inquiry_result_common(addr, psrm, cod, clk, 127, NULL);
    }
}

static void bt_handle_inquiry_result_rssi(const uint8_t* payload, size_t len) {
    uint8_t num;
    size_t i;
    size_t off_addr;
    size_t off_psrm;
    size_t off_cod;
    size_t off_clk;
    size_t off_rssi;

    if (len < 1) {
        return;
    }

    num = payload[0];
    off_addr = 1;
    off_psrm = off_addr + (size_t)num * 6;
    off_cod = off_psrm + (size_t)num * 2;
    off_clk = off_cod + (size_t)num * 3;
    off_rssi = off_clk + (size_t)num * 2;

    if (len < off_rssi + num) {
        return;
    }

    for (i = 0; i < num; ++i) {
        const uint8_t* addr = payload + off_addr + i * 6;
        uint8_t psrm = payload[off_psrm + i];
        uint32_t cod = (uint32_t)payload[off_cod + i * 3] |
            ((uint32_t)payload[off_cod + i * 3 + 1] << 8) |
            ((uint32_t)payload[off_cod + i * 3 + 2] << 16);
        uint16_t clk = (uint16_t)payload[off_clk + i * 2] |
            ((uint16_t)payload[off_clk + i * 2 + 1] << 8);
        int8_t rssi = (int8_t)payload[off_rssi + i];

        bt_handle_inquiry_result_common(addr, psrm, cod, clk, rssi, NULL);
    }
}

static void bt_handle_extended_inquiry_result(const uint8_t* payload, size_t len) {
    char name[64];
    uint32_t cod;
    uint16_t clk;

    if (len < 255 || payload[0] == 0) {
        return;
    }

    cod = (uint32_t)payload[9] |
        ((uint32_t)payload[10] << 8) |
        ((uint32_t)payload[11] << 16);
    clk = (uint16_t)payload[12] | ((uint16_t)payload[13] << 8);
    bt_parse_eir_name(payload + 15, len - 15, name, sizeof(name));
    bt_handle_inquiry_result_common(payload + 1, payload[7], cod, clk, (int8_t)payload[14], name);
}

static void bt_handle_remote_name_complete(const uint8_t* payload, size_t len) {
    bt_device_t* dev;
    char addr[24];
    size_t i;

    if (len < 7) {
        return;
    }

    dev = bt_find_device(payload + 1, true);
    if (dev == NULL) {
        return;
    }

    memset(dev->name, 0, sizeof(dev->name));
    for (i = 0; i < sizeof(dev->name) - 1 && (i + 7) < len; ++i) {
        unsigned char ch = payload[7 + i];
        if (ch == 0) {
            break;
        }
        dev->name[i] = isprint(ch) ? (char)ch : '.';
    }
    bt_trim_name(dev->name);
    bt_addr_to_str(dev->addr, addr, sizeof(addr));
    bt_emit("name %s status=%u value=%s\n", addr, payload[0], dev->name[0] ? dev->name : "-");
}

static void bt_handle_connection_complete(const uint8_t* payload, size_t len) {
    bt_device_t* dev;
    uint8_t status;
    uint16_t handle;
    char addr[24];

    if (len < 11) {
        return;
    }

    status = payload[0];
    handle = (uint16_t)payload[1] | ((uint16_t)payload[2] << 8);
    dev = bt_find_device(payload + 3, true);
    if (dev == NULL) {
        return;
    }

    bt_addr_to_str(dev->addr, addr, sizeof(addr));
    if (status == 0) {
        dev->connected = true;
        dev->handle = handle;
        bt_known_touch_from_device(dev);
        bt_emit("connect_ok %s handle=0x%04X\n", addr, handle);
        if (_pending.type == BT_PENDING_PAIR && bt_addr_equal(_pending.addr, dev->addr)) {
            _pending.handle = handle;
            bt_hci_auth_request(handle);
            bt_emit("pair_wait_auth %s\n", addr);
        }
        else if (_pending.type == BT_PENDING_CONNECT && bt_addr_equal(_pending.addr, dev->addr)) {
            bt_clear_pending();
        }
        /* a pointing-class peripheral (CoD major 0x05, minor bits 0x80):
           bring up the HID channels right away, standard mice accept the
           host-initiated PSM 0x0011/0x0013 pair */
        if (((dev->class_of_device >> 8) & 0x1f) == 0x05 &&
                (dev->class_of_device & 0xc0) != 0) {
            bt_hid_start(handle, dev->addr);
        }
    }
    else {
        bt_emit("connect_fail %s status=%u\n", addr, status);
        if (bt_pending_matches_addr(dev->addr)) {
            bt_clear_pending();
        }
    }
}

static void bt_handle_disconnection_complete(const uint8_t* payload, size_t len) {
    bt_device_t* dev;
    uint16_t handle;
    char addr[24];

    if (len < 4) {
        return;
    }

    handle = (uint16_t)payload[1] | ((uint16_t)payload[2] << 8);
    dev = bt_find_device_by_handle(handle);
    if (dev == NULL) {
        bt_emit("disconnect handle=0x%04X status=%u reason=%u\n",
            handle, payload[0], payload[3]);
        bt_hid_link_closed(handle, "acl_disconnect");
        return;
    }

    dev->connected = false;
    dev->handle = 0;
    bt_addr_to_str(dev->addr, addr, sizeof(addr));
    bt_emit("disconnect %s status=%u reason=%u\n", addr, payload[0], payload[3]);
    bt_hid_link_closed(handle, "acl_disconnect");
    if (_pending.handle == handle) {
        bt_clear_pending();
    }
}

static void bt_handle_auth_complete(const uint8_t* payload, size_t len) {
    bt_device_t* dev;
    uint16_t handle;
    char addr[24];

    if (len < 3) {
        return;
    }

    handle = (uint16_t)payload[1] | ((uint16_t)payload[2] << 8);
    dev = bt_find_device_by_handle(handle);
    if (dev == NULL) {
        return;
    }

    bt_addr_to_str(dev->addr, addr, sizeof(addr));
    if (payload[0] == 0) {
        bt_known_touch_from_device(dev);
        bt_emit("pair_ok %s handle=0x%04X\n", addr, handle);
    }
    else {
        bt_emit("pair_fail %s status=%u\n", addr, payload[0]);
    }
    if (_pending.type == BT_PENDING_PAIR && _pending.handle == handle) {
        bt_clear_pending();
    }
}

static void bt_handle_pin_code_request(const uint8_t* payload, size_t len) {
    uint8_t params[23];
    char addr[24];

    if (len < 6) {
        return;
    }

    bt_addr_to_str(payload, addr, sizeof(addr));
    if (_pending.type != BT_PENDING_PAIR || !bt_pending_matches_addr(payload)) {
        bt_hci_send_command(HCI_OGF_LINK_CTRL, HCI_OCF_PIN_CODE_REQ_NEG_REPLY, payload, 6);
        bt_emit("pair_reject %s reason=no_pin\n", addr);
        return;
    }

    memset(params, 0, sizeof(params));
    memcpy(params, payload, 6);
    params[6] = (uint8_t)strlen(_pending.pin);
    memcpy(params + 7, _pending.pin, params[6]);
    bt_hci_send_command(HCI_OGF_LINK_CTRL, HCI_OCF_PIN_CODE_REQ_REPLY, params, sizeof(params));
    bt_emit("pair_pin %s len=%u\n", addr, params[6]);
}

static void bt_handle_link_key_request(const uint8_t* payload, size_t len) {
    bt_device_t* dev;
    uint8_t params[22];
    char addr[24];

    if (len < 6) {
        return;
    }

    dev = bt_find_device(payload, false);
    bt_addr_to_str(payload, addr, sizeof(addr));
    if (dev == NULL || !dev->has_link_key) {
        bt_hci_send_command(HCI_OGF_LINK_CTRL, HCI_OCF_LINK_KEY_REQ_NEG_REPLY, payload, 6);
        bt_emit("link_key_miss %s\n", addr);
        return;
    }

    memcpy(params, payload, 6);
    memcpy(params + 6, dev->link_key, 16);
    bt_hci_send_command(HCI_OGF_LINK_CTRL, HCI_OCF_LINK_KEY_REQ_REPLY, params, sizeof(params));
    bt_emit("link_key_use %s\n", addr);
}

static void bt_handle_link_key_notify(const uint8_t* payload, size_t len) {
    bt_device_t* dev;
    char addr[24];

    if (len < 23) {
        return;
    }

    dev = bt_find_device(payload, true);
    if (dev == NULL) {
        return;
    }

    memcpy(dev->link_key, payload + 6, 16);
    dev->has_link_key = true;
    bt_known_touch_from_device(dev);
    bt_addr_to_str(dev->addr, addr, sizeof(addr));
    bt_emit("link_key_saved %s type=%u\n", addr, payload[22]);
}

static void bt_handle_conn_request(const uint8_t* payload, size_t len) {
    char addr[24];

    if (len < 10) {
        return;
    }

    bt_addr_to_str(payload, addr, sizeof(addr));
    bt_emit("incoming_connect %s class=0x%02X%02X%02X link=%u\n",
        addr, payload[8], payload[7], payload[6], payload[9]);
    /* accept so a reconnecting mouse (or any known device paging us)
       gets its ACL link back; role 0x01 keeps us slave */
    bt_hci_accept_connection(payload);
}

static void bt_handle_io_capability_request(const uint8_t* payload, size_t len) {
    uint8_t params[9];
    char addr[24];

    if (len < 6) {
        return;
    }

    memset(params, 0, sizeof(params));
    memcpy(params, payload, 6);
    params[6] = 0x03;
    params[7] = 0x00;
    params[8] = 0x01;
    bt_hci_send_command(HCI_OGF_LINK_CTRL, HCI_OCF_IO_CAPABILITY_REQ_REPLY, params, sizeof(params));
    bt_addr_to_str(payload, addr, sizeof(addr));
    bt_emit("pair_io_cap %s capability=noinput\n", addr);
}

static void bt_handle_user_confirmation_request(const uint8_t* payload, size_t len) {
    char addr[24];

    if (len < 10) {
        return;
    }

    bt_addr_to_str(payload, addr, sizeof(addr));
    if (_pending.type == BT_PENDING_PAIR && bt_pending_matches_addr(payload)) {
        bt_hci_send_command(HCI_OGF_LINK_CTRL, HCI_OCF_USER_CONFIRM_REQ_REPLY, payload, 6);
        bt_emit("pair_confirm %s auto=yes\n", addr);
    }
    else {
        bt_hci_send_command(HCI_OGF_LINK_CTRL, HCI_OCF_USER_CONFIRM_REQ_NEG_REPLY, payload, 6);
        bt_emit("pair_confirm %s auto=no\n", addr);
    }
}

static void bt_handle_user_passkey_request(const uint8_t* payload, size_t len) {
    uint8_t params[10];
    char* endptr;
    unsigned long passkey;
    char addr[24];

    if (len < 6) {
        return;
    }

    bt_addr_to_str(payload, addr, sizeof(addr));
    if (_pending.type != BT_PENDING_PAIR || !bt_pending_matches_addr(payload)) {
        bt_hci_send_command(HCI_OGF_LINK_CTRL, HCI_OCF_USER_PASSKEY_REQ_NEG_REPLY, payload, 6);
        bt_emit("pair_passkey %s auto=no\n", addr);
        return;
    }

    passkey = strtoul(_pending.pin, &endptr, 10);
    if (*_pending.pin == 0 || *endptr != 0 || passkey > 999999UL) {
        bt_hci_send_command(HCI_OGF_LINK_CTRL, HCI_OCF_USER_PASSKEY_REQ_NEG_REPLY, payload, 6);
        bt_emit("pair_passkey %s auto=no\n", addr);
        return;
    }

    memset(params, 0, sizeof(params));
    memcpy(params, payload, 6);
    params[6] = (uint8_t)(passkey & 0xff);
    params[7] = (uint8_t)((passkey >> 8) & 0xff);
    params[8] = (uint8_t)((passkey >> 16) & 0xff);
    params[9] = (uint8_t)((passkey >> 24) & 0xff);
    bt_hci_send_command(HCI_OGF_LINK_CTRL, HCI_OCF_USER_PASSKEY_REQ_REPLY, params, sizeof(params));
    bt_emit("pair_passkey %s auto=yes value=%lu", addr, passkey);
}

static void bt_handle_simple_pairing_complete(const uint8_t* payload, size_t len) {
    char addr[24];

    if (len < 7) {
        return;
    }

    bt_addr_to_str(payload + 1, addr, sizeof(addr));
    bt_emit("pair_complete %s status=%u", addr, payload[0]);
}

/* ---------------- host->controller ACL data path ---------------- */

static int bt_hci_send_acl(uint16_t handle, const uint8_t* data, size_t len) {
    uint8_t pkt[4 + 264];

    if (len > 260) {
        return -1;
    }
    pkt[0] = (uint8_t)(handle & 0xff);
    /* PB=0b10: first automatically-flushable packet of a new L2CAP PDU
       (our outbound PDUs always fit a single ACL packet) */
    pkt[1] = (uint8_t)(((handle >> 8) & 0x0f) | 0x20);
    pkt[2] = (uint8_t)(len & 0xff);
    pkt[3] = (uint8_t)(len >> 8);
    memcpy(pkt + 4, data, len);

    if (_acl_credits > 0) {
        --_acl_credits;
    }
    else {
        /* out of controller buffer credits: still push the packet (the
           uart's hardware flow control protects the transport) but say
           so - sustained credit exhaustion would mean lost reports */
        slog("bluetooth acl no_credits handle=0x%04x\n", handle);
    }
    return bt_hci_send_packet(HCI_PKT_ACL, pkt, 4 + len);
}

static void bt_handle_num_completed_pkts(const uint8_t* payload, size_t len) {
    uint8_t num;
    size_t i;

    if (len < 1) {
        return;
    }
    num = payload[0];
    for (i = 0; i < num && 1 + (i + 1) * 4 <= len; ++i) {
        uint16_t completed = (uint16_t)payload[3 + i * 4] |
            ((uint16_t)payload[4 + i * 4] << 8);
        _acl_credits = (uint16_t)(_acl_credits + completed);
    }
}

static int bt_hci_command_sync_ret(uint16_t ogf, uint16_t ocf,
        const uint8_t* params, uint8_t param_len, uint32_t timeout_ms,
        uint8_t* ret_params, uint8_t ret_cap, uint8_t* ret_len) {
    uint16_t opcode = HCI_OPCODE(ogf, ocf);
    int status;

    if (ret_len != NULL) {
        *ret_len = 0;
    }
    if (bt_hci_send_command_raw(opcode, params, param_len) != 0) {
        slog("bluetooth cmd send_failed opcode=0x%04x\n", opcode);
        return -1;
    }
    status = bt_wait_for_opcode(opcode, timeout_ms);
    if (status == 0 && ret_params != NULL && ret_len != NULL && _wait_cmd.got_ret) {
        uint8_t n = _wait_cmd.ret_len;
        if (n > ret_cap) {
            n = ret_cap;
        }
        memcpy(ret_params, _wait_cmd.ret, n);
        *ret_len = n;
    }
    return status;
}

/* ---------------- L2CAP (basic mode, HID PSMs only) ---------------- */

static l2cap_chan_t* l2cap_find_by_local(uint16_t cid) {
    int i;

    for (i = 0; i < MAX_L2CAP_CHANS; ++i) {
        if (_l2chans[i].used && _l2chans[i].local_cid == cid) {
            return &_l2chans[i];
        }
    }
    return NULL;
}

/* a config request names the peer's cid, which is still unknown while
   our own connection request is outstanding - fall back to the single
   pending channel on that link (HID only ever has ctrl + intr) */
static l2cap_chan_t* l2cap_find_by_remote(uint16_t handle, uint16_t cid) {
    int i;
    l2cap_chan_t* fallback = NULL;

    for (i = 0; i < MAX_L2CAP_CHANS; ++i) {
        if (!_l2chans[i].used || _l2chans[i].acl_handle != handle) {
            continue;
        }
        if (_l2chans[i].remote_cid == cid) {
            return &_l2chans[i];
        }
        if (cid == 0 && _l2chans[i].state == L2CAP_STATE_CONN_REQ_SENT &&
                fallback == NULL) {
            fallback = &_l2chans[i];
        }
    }
    return fallback;
}

static l2cap_chan_t* l2cap_find_any(uint16_t handle, uint16_t psm) {
    int i;

    for (i = 0; i < MAX_L2CAP_CHANS; ++i) {
        if (_l2chans[i].used && _l2chans[i].acl_handle == handle &&
                _l2chans[i].psm == psm) {
            return &_l2chans[i];
        }
    }
    return NULL;
}

static uint16_t l2cap_alloc_cid(void) {
    uint16_t cid = _l2_next_cid++;

    if (_l2_next_cid >= 0xfff0) { /* keep clear of the reserved top range */
        _l2_next_cid = 0x0040;
    }
    return cid;
}

static uint8_t l2cap_next_sig_id(void) {
    uint8_t id = _l2_next_sig_id++;

    if (_l2_next_sig_id == 0) {
        _l2_next_sig_id = 1;
    }
    return id;
}

/* one L2CAP PDU wrapped into a single ACL packet (PB=0b10) */
static int l2cap_send_pdu(uint16_t handle, uint16_t cid,
        const uint8_t* payload, uint16_t payload_len) {
    uint8_t pdu[4 + 256];

    if (payload_len > 256) {
        return -1;
    }
    pdu[0] = (uint8_t)(payload_len & 0xff);
    pdu[1] = (uint8_t)(payload_len >> 8);
    pdu[2] = (uint8_t)(cid & 0xff);
    pdu[3] = (uint8_t)(cid >> 8);
    if (payload_len > 0 && payload != NULL) {
        memcpy(pdu + 4, payload, payload_len);
    }
    return bt_hci_send_acl(handle, pdu, 4 + payload_len);
}

static int l2cap_send_signal(uint16_t handle, uint8_t code, uint8_t id,
        const uint8_t* data, uint8_t data_len) {
    uint8_t cmd[4 + 64];

    if (data_len > 64) {
        return -1;
    }
    cmd[0] = code;
    cmd[1] = id;
    cmd[2] = data_len;
    cmd[3] = 0;
    if (data_len > 0) {
        memcpy(cmd + 4, data, data_len);
    }
    return l2cap_send_pdu(handle, L2CAP_CID_SIGNAL, cmd, 4 + data_len);
}

static void l2cap_send_conn_req(l2cap_chan_t* ch) {
    uint8_t data[4];

    ch->sig_id = l2cap_next_sig_id();
    data[0] = (uint8_t)(ch->psm & 0xff);
    data[1] = (uint8_t)(ch->psm >> 8);
    data[2] = (uint8_t)(ch->local_cid & 0xff);
    data[3] = (uint8_t)(ch->local_cid >> 8);
    l2cap_send_signal(ch->acl_handle, L2CAP_SIG_CONN_REQ, ch->sig_id, data, sizeof(data));
}

/* request the default MTU and accept whatever options the device asks
   for on the other direction (boot mouse reports are a few bytes) */
static void l2cap_send_conf_req(l2cap_chan_t* ch) {
    uint8_t data[8];

    ch->sig_id = l2cap_next_sig_id();
    data[0] = (uint8_t)(ch->remote_cid & 0xff);
    data[1] = (uint8_t)(ch->remote_cid >> 8);
    data[2] = 0;
    data[3] = 0;
    data[4] = 0x01; /* option: MTU */
    data[5] = 0x02;
    data[6] = (uint8_t)(L2CAP_MTU_DEFAULT & 0xff);
    data[7] = (uint8_t)(L2CAP_MTU_DEFAULT >> 8);
    ch->conf_req_sent = true;
    l2cap_send_signal(ch->acl_handle, L2CAP_SIG_CONF_REQ, ch->sig_id, data, sizeof(data));
}

static void l2cap_send_conf_rsp(l2cap_chan_t* ch, uint8_t id, uint16_t result) {
    uint8_t data[6];

    data[0] = (uint8_t)(ch->remote_cid & 0xff);
    data[1] = (uint8_t)(ch->remote_cid >> 8);
    data[2] = 0;
    data[3] = 0;
    data[4] = (uint8_t)(result & 0xff);
    data[5] = (uint8_t)(result >> 8);
    ch->conf_rsp_sent = true;
    l2cap_send_signal(ch->acl_handle, L2CAP_SIG_CONF_RSP, id, data, sizeof(data));
}

static void l2cap_send_conn_rsp(uint16_t handle, uint8_t id, uint16_t dcid,
        uint16_t scid, uint16_t result) {
    uint8_t data[8];

    data[0] = (uint8_t)(dcid & 0xff);
    data[1] = (uint8_t)(dcid >> 8);
    data[2] = (uint8_t)(scid & 0xff);
    data[3] = (uint8_t)(scid >> 8);
    data[4] = (uint8_t)(result & 0xff);
    data[5] = (uint8_t)(result >> 8);
    data[6] = 0; /* status */
    data[7] = 0;
    l2cap_send_signal(handle, L2CAP_SIG_CONN_RSP, id, data, sizeof(data));
}

static bool bt_hid_chan_up(void) {
    return _hid.active && _hid.ctrl != NULL && _hid.intr != NULL &&
        _hid.ctrl->state == L2CAP_STATE_OPEN && _hid.intr->state == L2CAP_STATE_OPEN;
}

static void bt_hid_check_up(void) {
    char addr[24];

    if (!_hid.active || _hid.up || !bt_hid_chan_up()) {
        return;
    }
    _hid.up = true;
    bt_addr_to_str(_hid.addr, addr, sizeof(addr));
    bt_emit("hid_ok %s handle=0x%04X\n", addr, _hid.acl_handle);
    slog("bluetooth hid link up %s ctrl=0x%04x intr=0x%04x\n",
        addr, _hid.ctrl->local_cid, _hid.intr->local_cid);
}

/* both channels of one link are gone: forget the HID session */
static void bt_hid_link_closed(uint16_t handle, const char* reason) {
    char addr[24];
    bool was_up;

    if (!_hid.active || _hid.acl_handle != handle) {
        return;
    }
    was_up = _hid.up;
    bt_addr_to_str(_hid.addr, addr, sizeof(addr));
    memset(&_hid, 0, sizeof(_hid));
    if (was_up) {
        bt_emit("hid_disconnect %s reason=%s\n", addr, reason);
    }
}

static void bt_hid_stop(void) {
    if (!_hid.active) {
        return;
    }
    if (_hid.ctrl != NULL) {
        l2cap_chan_close(_hid.ctrl, true);
    }
    if (_hid.intr != NULL) {
        l2cap_chan_close(_hid.intr, true);
    }
    bt_hid_link_closed(_hid.acl_handle, "closed");
}

/* open (or reuse) one channel of the HID pair on an established link;
   a reconnecting mouse may already have opened one side itself, which
   l2cap_chan_open detects and leaves alone */
static l2cap_chan_t* l2cap_chan_open(uint16_t handle, uint16_t psm) {
    l2cap_chan_t* ch = l2cap_find_any(handle, psm);
    int i;

    if (ch != NULL) {
        if (ch->state == L2CAP_STATE_OPEN) {
            return ch;
        }
        if (ch->state == L2CAP_STATE_CLOSED || ch->state == L2CAP_STATE_CLOSING) {
            /* stale slot from a dead session: re-arm it */
            uint16_t keep_cid = ch->local_cid;
            memset(ch, 0, sizeof(*ch));
            ch->used = true;
            ch->local_cid = keep_cid;
        }
        else {
            return ch; /* setup already in flight */
        }
    }
    else {
        for (i = 0; i < MAX_L2CAP_CHANS; ++i) {
            if (!_l2chans[i].used) {
                ch = &_l2chans[i];
                break;
            }
        }
        if (ch == NULL) {
            slog("bluetooth l2cap no_channel handle=0x%04x psm=0x%04x\n", handle, psm);
            return NULL;
        }
        memset(ch, 0, sizeof(*ch));
        ch->used = true;
        ch->local_cid = l2cap_alloc_cid();
    }

    ch->acl_handle = handle;
    ch->psm = psm;
    ch->state = L2CAP_STATE_CONN_REQ_SENT;
    ch->retries = 0;
    ch->retry_ms = kernel_tic_ms(0) + L2CAP_STEP_TIMEOUT_MS;
    l2cap_send_conn_req(ch);
    return ch;
}

static void l2cap_chan_close(l2cap_chan_t* ch, bool send_req) {
    if (ch == NULL || !ch->used) {
        return;
    }
    if (_hid.ctrl == ch) {
        _hid.ctrl = NULL;
    }
    if (_hid.intr == ch) {
        _hid.intr = NULL;
    }
    if (send_req && ch->state != L2CAP_STATE_CLOSED && ch->remote_cid != 0) {
        uint8_t data[4];

        data[0] = (uint8_t)(ch->remote_cid & 0xff);
        data[1] = (uint8_t)(ch->remote_cid >> 8);
        data[2] = (uint8_t)(ch->local_cid & 0xff);
        data[3] = (uint8_t)(ch->local_cid >> 8);
        l2cap_send_signal(ch->acl_handle, L2CAP_SIG_DISCONN_REQ,
            l2cap_next_sig_id(), data, sizeof(data));
        ch->state = L2CAP_STATE_CLOSING;
        ch->retry_ms = kernel_tic_ms(0) + L2CAP_STEP_TIMEOUT_MS;
        return;
    }
    memset(ch, 0, sizeof(*ch));
}

/* drive every channel's handshake forward; called from bt_loop and from
   the synchronous command-wait pump so setup progresses either way */
static void l2cap_step(void) {
    uint64_t now = kernel_tic_ms(0);
    int i;

    for (i = 0; i < MAX_L2CAP_CHANS; ++i) {
        l2cap_chan_t* ch = &_l2chans[i];

        if (!ch->used) {
            continue;
        }
        switch (ch->state) {
        case L2CAP_STATE_CONN_REQ_SENT:
            if (now >= ch->retry_ms) {
                if (++ch->retries > L2CAP_STEP_MAX_RETRIES) {
                    slog("bluetooth l2cap conn_req_timeout psm=0x%04x\n", ch->psm);
                    l2cap_chan_close(ch, false);
                    break;
                }
                ch->retry_ms = now + L2CAP_STEP_TIMEOUT_MS;
                l2cap_send_conn_req(ch);
            }
            break;
        case L2CAP_STATE_CONF_SENT:
            if (ch->conf_rsp_recv && ch->conf_req_recv) {
                ch->state = L2CAP_STATE_OPEN;
                if (ch->psm == L2CAP_PSM_HID_CTRL && _hid.active &&
                        _hid.ctrl == ch) {
                    /* standard HID host move: pin the mouse to boot
                       protocol so reports are [btn, dx, dy, (wheel)] */
                    uint8_t hidp = HIDP_TRANS_SET_PROTOCOL | HIDP_PROTOCOL_BOOT;
                    l2cap_send_pdu(ch->acl_handle, ch->remote_cid, &hidp, 1);
                }
                bt_hid_check_up();
            }
            else if (now >= ch->retry_ms) {
                if (++ch->retries > L2CAP_STEP_MAX_RETRIES) {
                    slog("bluetooth l2cap config_timeout psm=0x%04x\n", ch->psm);
                    l2cap_chan_close(ch, true);
                    break;
                }
                ch->retry_ms = now + L2CAP_STEP_TIMEOUT_MS;
                if (!ch->conf_req_sent) {
                    l2cap_send_conf_req(ch);
                }
            }
            break;
        case L2CAP_STATE_CLOSING:
            if (now >= ch->retry_ms) {
                l2cap_chan_close(ch, false);
            }
            break;
        default:
            break;
        }
    }
}

/* ---------------- inbound L2CAP signaling ---------------- */

static void l2cap_handle_conn_req(uint16_t handle, uint8_t id,
        const uint8_t* data, size_t len) {
    uint16_t psm;
    uint16_t rcid;
    l2cap_chan_t* ch;
    int i;

    if (len < 4) {
        return;
    }
    psm = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    rcid = (uint16_t)data[2] | ((uint16_t)data[3] << 8);

    if (psm != L2CAP_PSM_HID_CTRL && psm != L2CAP_PSM_HID_INTR) {
        l2cap_send_conn_rsp(handle, id, 0, rcid, L2CAP_CONN_PSM_UNSUPPORTED);
        return;
    }

    /* a reconnecting mouse may open the channels itself: reuse a live
       slot for the same psm instead of allocating a second one */
    ch = l2cap_find_any(handle, psm);
    if (ch == NULL) {
        for (i = 0; i < MAX_L2CAP_CHANS; ++i) {
            if (!_l2chans[i].used) {
                ch = &_l2chans[i];
                break;
            }
        }
    }
    if (ch == NULL) {
        l2cap_send_conn_rsp(handle, id, 0, rcid, 0x0004); /* no resources */
        return;
    }

    memset(ch, 0, sizeof(*ch));
    ch->used = true;
    ch->acl_handle = handle;
    ch->psm = psm;
    ch->local_cid = l2cap_alloc_cid();
    ch->remote_cid = rcid;
    ch->state = L2CAP_STATE_CONF_SENT;
    ch->retries = 0;
    ch->retry_ms = kernel_tic_ms(0) + L2CAP_STEP_TIMEOUT_MS;
    l2cap_send_conn_rsp(handle, id, ch->local_cid, rcid, L2CAP_CONN_SUCCESS);
    /* send our config request right away; the device's own request is
       answered in l2cap_handle_conf_req */
    l2cap_send_conf_req(ch);

    /* attach to (or start) the HID session on this link */
    if (!_hid.active) {
        bt_device_t* dev = bt_find_device_by_handle(handle);
        bt_hid_start(handle, dev != NULL ? dev->addr : NULL);
    }
    if (_hid.active && _hid.acl_handle == handle) {
        if (psm == L2CAP_PSM_HID_CTRL) {
            _hid.ctrl = ch;
        }
        else {
            _hid.intr = ch;
        }
    }
}

static void l2cap_handle_conn_rsp(uint16_t handle, uint8_t id,
        const uint8_t* data, size_t len) {
    uint16_t dcid;
    uint16_t scid;
    uint16_t result;
    l2cap_chan_t* ch;

    (void)handle;
    if (len < 8) {
        return;
    }
    dcid = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    scid = (uint16_t)data[2] | ((uint16_t)data[3] << 8);
    result = (uint16_t)data[4] | ((uint16_t)data[5] << 8);

    ch = l2cap_find_by_local(scid);
    if (ch == NULL || ch->state != L2CAP_STATE_CONN_REQ_SENT || ch->sig_id != id) {
        return;
    }
    if (result != L2CAP_CONN_SUCCESS) {
        slog("bluetooth l2cap conn_refused psm=0x%04x result=%u\n", ch->psm, result);
        l2cap_chan_close(ch, false);
        return;
    }
    ch->remote_cid = dcid;
    ch->state = L2CAP_STATE_CONF_SENT;
    ch->retries = 0;
    ch->retry_ms = kernel_tic_ms(0) + L2CAP_STEP_TIMEOUT_MS;
    l2cap_send_conf_req(ch);
}

/* accept the device's options verbatim (boot mouse payloads are tiny,
   any default MTU works) */
static void l2cap_handle_conf_req(uint16_t handle, uint8_t id,
        const uint8_t* data, size_t len) {
    uint16_t dcid;
    l2cap_chan_t* ch;

    if (len < 4) {
        return;
    }
    dcid = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    ch = l2cap_find_by_remote(handle, dcid);
    if (ch == NULL) {
        uint8_t rsp[6];

        rsp[0] = (uint8_t)(dcid & 0xff);
        rsp[1] = (uint8_t)(dcid >> 8);
        rsp[2] = 0;
        rsp[3] = 0;
        rsp[4] = 0x02; /* failure: unknown dcid */
        rsp[5] = 0x00;
        l2cap_send_signal(handle, L2CAP_SIG_CONF_RSP, id, rsp, sizeof(rsp));
        return;
    }
    ch->conf_req_recv = true;
    l2cap_send_conf_rsp(ch, id, L2CAP_CONF_SUCCESS);
    if (ch->state == L2CAP_STATE_CONN_REQ_SENT && ch->remote_cid == 0) {
        /* their config request raced our connection response: adopt the
           source cid it carries */
        ch->remote_cid = dcid;
    }
}

static void l2cap_handle_conf_rsp(uint16_t handle, uint8_t id,
        const uint8_t* data, size_t len) {
    uint16_t scid;
    uint16_t result;
    l2cap_chan_t* ch;
    int i;

    (void)handle;
    if (len < 6) {
        return;
    }
    scid = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    result = (uint16_t)data[4] | ((uint16_t)data[5] << 8);

    ch = l2cap_find_by_local(scid);
    if (ch == NULL) {
        return;
    }
    for (i = 0; i < MAX_L2CAP_CHANS; ++i) {
        if (&_l2chans[i] == ch) {
            break;
        }
    }
    if (result != L2CAP_CONF_SUCCESS) {
        slog("bluetooth l2cap conf_refused psm=0x%04x result=%u\n", ch->psm, result);
        l2cap_chan_close(ch, true);
        return;
    }
    if (id == ch->sig_id) {
        ch->conf_rsp_recv = true;
    }
}

static void l2cap_handle_disconn_req(uint16_t handle, uint8_t id,
        const uint8_t* data, size_t len) {
    uint16_t dcid;
    uint16_t scid;
    l2cap_chan_t* ch;

    if (len < 4) {
        return;
    }
    dcid = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    scid = (uint16_t)data[2] | ((uint16_t)data[3] << 8);
    ch = l2cap_find_by_local(dcid);
    /* echo the request back as the response (spec: same dcid/scid) */
    l2cap_send_signal(handle, L2CAP_SIG_DISCONN_RSP, id, data, 4);
    if (ch != NULL) {
        l2cap_chan_close(ch, false);
        if (_hid.active && _hid.acl_handle == handle &&
                _hid.ctrl == NULL && _hid.intr == NULL) {
            bt_hid_link_closed(handle, "l2cap_disconnect");
        }
    }
    (void)scid;
}

static void l2cap_handle_signal(uint16_t handle, const uint8_t* pdu, size_t len) {
    size_t off = 0;

    while (off + 4 <= len) {
        uint8_t code = pdu[off];
        uint8_t id = pdu[off + 1];
        uint16_t clen = (uint16_t)pdu[off + 2] | ((uint16_t)pdu[off + 3] << 8);
        const uint8_t* data = pdu + off + 4;

        if (off + 4 + clen > len) {
            break;
        }
        switch (code) {
        case L2CAP_SIG_CONN_REQ:
            l2cap_handle_conn_req(handle, id, data, clen);
            break;
        case L2CAP_SIG_CONN_RSP:
            l2cap_handle_conn_rsp(handle, id, data, clen);
            break;
        case L2CAP_SIG_CONF_REQ:
            l2cap_handle_conf_req(handle, id, data, clen);
            break;
        case L2CAP_SIG_CONF_RSP:
            l2cap_handle_conf_rsp(handle, id, data, clen);
            break;
        case L2CAP_SIG_DISCONN_REQ:
            l2cap_handle_disconn_req(handle, id, data, clen);
            break;
        case L2CAP_SIG_DISCONN_RSP:
            {
                l2cap_chan_t* ch = NULL;
                int i;
                if (clen >= 4) {
                    uint16_t dcid = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
                    for (i = 0; i < MAX_L2CAP_CHANS; ++i) {
                        if (_l2chans[i].used && _l2chans[i].local_cid == dcid) {
                            ch = &_l2chans[i];
                            break;
                        }
                    }
                }
                if (ch != NULL) {
                    l2cap_chan_close(ch, false);
                }
            }
            break;
        case L2CAP_SIG_ECHO_REQ:
            /* answer with the same data so link supervision works */
            l2cap_send_signal(handle, L2CAP_SIG_ECHO_RSP, id, data,
                clen > 64 ? 64 : (uint8_t)clen);
            break;
        default:
            break;
        }
        off += 4 + clen;
    }
}

/* ---------------- HIDP ---------------- */

/* boot mouse report: [buttons, dx, dy, (wheel)] - fan it out to the
   mouse subscribers in the /dev/hid0 7-byte event format */
static void bt_hid_handle_report(const uint8_t* data, size_t len) {
    uint8_t evt[BT_SUBSCRIBER_EVENT_SIZE];

    if (len < 3) {
        return;
    }
    memset(evt, 0, sizeof(evt));
    evt[0] = data[0]; /* buttons: bit0 left, bit1 right, bit2 middle */
    evt[1] = data[1]; /* dx (signed) */
    evt[2] = data[2]; /* dy (signed) */
    if (len >= 4) {
        evt[3] = data[3]; /* wheel on report-protocol mice */
    }
    bt_hid_dispatch_mouse(evt);
}

static void bt_hid_handle_ctrl(l2cap_chan_t* ch, const uint8_t* data, size_t len) {
    char addr[24];

    if (len < 1) {
        return;
    }
    switch (data[0] & 0xF0) {
    case HIDP_TRANS_HANDSHAKE:
        if (!_hid.active || _hid.ctrl != ch) {
            break;
        }
        if (data[0] == HIDP_HANDSHAKE_SUCCESS) {
            _hid.boot_protocol_ok = true;
            slog("bluetooth hid boot_protocol ok\n");
        }
        else {
            /* not a boot device or busy: reports keep arriving in
               report protocol, which for a plain mouse has the same
               [btn, dx, dy, (wheel)] layout */
            slog("bluetooth hid set_protocol refused=0x%02x\n", data[0] & 0x0f);
        }
        break;
    case HIDP_TRANS_HID_CONTROL:
        if (data[0] == HIDP_HID_CONTROL_VC_UNPLUG) {
            /* the mouse wants its virtual cable unplugged: close the
               channels and the ACL link, the pairing itself survives */
            bt_addr_to_str(_hid.addr, addr, sizeof(addr));
            bt_hid_stop();
            if (ch->acl_handle != 0) {
                bt_hci_disconnect(ch->acl_handle);
            }
            bt_emit("hid_unplug %s\n", addr);
        }
        break;
    default:
        break;
    }
}

static void l2cap_chan_data(l2cap_chan_t* ch, const uint8_t* payload, size_t len) {
    if (ch->psm == L2CAP_PSM_HID_CTRL) {
        bt_hid_handle_ctrl(ch, payload, len);
    }
    else if (ch->psm == L2CAP_PSM_HID_INTR) {
        if (len >= 1 && payload[0] == HIDP_DATA_INPUT) {
            bt_hid_handle_report(payload + 1, len - 1);
        }
    }
}

static void l2cap_dispatch(uint16_t handle, const uint8_t* pdu, size_t len) {
    uint16_t pdu_len;
    uint16_t cid;
    l2cap_chan_t* ch;

    if (len < 4) {
        return;
    }
    pdu_len = (uint16_t)pdu[0] | ((uint16_t)pdu[1] << 8);
    cid = (uint16_t)pdu[2] | ((uint16_t)pdu[3] << 8);
    if (pdu_len > len - 4) {
        pdu_len = (uint16_t)(len - 4); /* truncated fragment: use what we have */
    }

    if (cid == L2CAP_CID_SIGNAL) {
        l2cap_handle_signal(handle, pdu + 4, pdu_len);
        return;
    }
    ch = l2cap_find_by_local(cid);
    if (ch == NULL || ch->acl_handle != handle) {
        return;
    }
    l2cap_chan_data(ch, pdu + 4, pdu_len);
}

/* ---------------- /dev/bt0 subscriber fan-out ----------------
   same wire protocol as usbhostd's /dev/hid0 (libs/usb/usbhidsrv.c):
   fcntl cmd 0 picks the report id, reads then pop fixed 7-byte events
   from the per-fd ring, and consumers park with proc_block_by(node) */

static bt_subscriber_t* bt_sub_find(int fd, int from_pid) {
    int i;

    for (i = 0; i < BT_SUBSCRIBER_MAX; ++i) {
        if (_subs[i].used && _subs[i].fd == fd && _subs[i].from_pid == from_pid) {
            return &_subs[i];
        }
    }
    return NULL;
}

static bt_subscriber_t* bt_sub_alloc(int fd, int from_pid) {
    int i;

    for (i = 0; i < BT_SUBSCRIBER_MAX; ++i) {
        if (!_subs[i].used) {
            memset(&_subs[i], 0, sizeof(_subs[i]));
            _subs[i].used = true;
            _subs[i].fd = fd;
            _subs[i].from_pid = from_pid;
            return &_subs[i];
        }
    }
    return NULL;
}

static void bt_sub_free(int fd, int from_pid) {
    bt_subscriber_t* sub = bt_sub_find(fd, from_pid);

    if (sub != NULL) {
        memset(sub, 0, sizeof(*sub));
    }
}

static bool sub_queue_has_data(const bt_subscriber_t* sub) {
    return sub->q_rd != sub->q_wr;
}

static void sub_queue_push(bt_subscriber_t* sub, const uint8_t* data, uint8_t len) {
    if (len > 8) {
        len = 8;
    }
    memcpy(sub->q_data[sub->q_wr], data, len);
    if (len < 8) {
        memset(sub->q_data[sub->q_wr] + len, 0, 8 - len);
    }
    sub->q_len[sub->q_wr] = len;
    sub->q_wr = (uint8_t)((sub->q_wr + 1u) % BT_SUBSCRIBER_QUEUE_DEPTH);
    if (sub->q_wr == sub->q_rd) {
        /* full: drop the oldest event */
        sub->q_rd = (uint8_t)((sub->q_rd + 1u) % BT_SUBSCRIBER_QUEUE_DEPTH);
    }
}

/* pop whole events while they fit (same batching as usbhidsrv) */
static int sub_queue_pop(bt_subscriber_t* sub, void* buf, int size) {
    uint8_t* dst = (uint8_t*)buf;
    int total = 0;

    while (sub_queue_has_data(sub)) {
        int len = sub->q_len[sub->q_rd];
        if (total + len > size) {
            break;
        }
        memcpy(dst + total, sub->q_data[sub->q_rd], len);
        total += len;
        sub->q_rd = (uint8_t)((sub->q_rd + 1u) % BT_SUBSCRIBER_QUEUE_DEPTH);
    }
    return total;
}

/* fan one mouse event out; wake each subscriber only on its queue's
   empty -> non-empty edge (directed proc_wakeup_by, same rationale as
   usbhid_dispatch_evt), the backlog re-assert in bt_loop covers a wake
   that got spent on a generic IPC wait */
static void bt_hid_dispatch_mouse(const uint8_t* evt) {
    int i;

    ipc_disable();
    for (i = 0; i < BT_SUBSCRIBER_MAX; ++i) {
        bt_subscriber_t* sub = &_subs[i];
        bool was_empty;

        if (!sub->used || sub->report_id != BT_REPORT_ID_MOUSE) {
            continue;
        }
        was_empty = !sub_queue_has_data(sub);
        sub_queue_push(sub, evt, BT_SUBSCRIBER_EVENT_SIZE);
        if (was_empty && _bt_dev != NULL && _bt_dev->mnt_info.node != 0) {
            proc_wakeup_by(sub->from_pid, _bt_dev->mnt_info.node);
            _sub_reassert_ms = kernel_tic_ms(0) + BT_HID_REASSERT_MS;
        }
    }
    ipc_enable();
}

static bool bt_hid_backlog(void) {
    int i;

    for (i = 0; i < BT_SUBSCRIBER_MAX; ++i) {
        if (_subs[i].used && _subs[i].report_id == BT_REPORT_ID_MOUSE &&
                sub_queue_has_data(&_subs[i])) {
            return true;
        }
    }
    return false;
}

static void bt_hid_rewake_backlog(void) {
    int i;

    if (_bt_dev == NULL || _bt_dev->mnt_info.node == 0) {
        return;
    }
    for (i = 0; i < BT_SUBSCRIBER_MAX; ++i) {
        if (_subs[i].used && _subs[i].report_id == BT_REPORT_ID_MOUSE &&
                sub_queue_has_data(&_subs[i])) {
            proc_wakeup_by(_subs[i].from_pid, _bt_dev->mnt_info.node);
        }
    }
}

static int bt_vdev_open(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        int oflag, void* p) {
    (void)dev;
    (void)node;
    (void)oflag;
    (void)p;
    if (fd < 0) {
        return -1;
    }
    return bt_sub_alloc(fd, from_pid) != NULL ? 0 : -1;
}

static int bt_vdev_close(vdevice_t* dev, int fd, int from_pid, ewokos_addr_t node,
        fsinfo_t* fsinfo, void* p) {
    (void)dev;
    (void)node;
    (void)fsinfo;
    (void)p;
    bt_sub_free(fd, from_pid);
    return 0;
}

static int bt_vdev_fcntl(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info,
        int cmd, proto_t* in, proto_t* out, void* p) {
    bt_subscriber_t* sub;

    (void)dev;
    (void)info;
    (void)out;
    (void)p;
    sub = bt_sub_find(fd, from_pid);
    if (sub == NULL) {
        return -1;
    }
    if (cmd == 0) {
        sub->report_id = (uint8_t)proto_read_int(in);
        sub->q_rd = 0;
        sub->q_wr = 0;
        return 0;
    }
    return -1;
}

static void bt_hid_start(uint16_t handle, const uint8_t* addr) {
    if (handle == 0) {
        return;
    }
    /* a stale session on a dead handle must not leak its channels */
    if (_hid.active && _hid.acl_handle != handle) {
        bt_hid_stop();
    }
    if (!_hid.active) {
        memset(&_hid, 0, sizeof(_hid));
        _hid.active = true;
        _hid.acl_handle = handle;
        if (addr != NULL) {
            memcpy(_hid.addr, addr, 6);
        }
    }
    if (_hid.ctrl == NULL || _hid.ctrl->state != L2CAP_STATE_OPEN) {
        _hid.ctrl = l2cap_chan_open(handle, L2CAP_PSM_HID_CTRL);
    }
    if (_hid.intr == NULL || _hid.intr->state != L2CAP_STATE_OPEN) {
        _hid.intr = l2cap_chan_open(handle, L2CAP_PSM_HID_INTR);
    }
}

/* drop every L2CAP channel (the ACL links die with the radio anyway) */
static void bt_hid_stack_reset(void) {
    memset(&_l2chans, 0, sizeof(_l2chans));
    memset(&_hid, 0, sizeof(_hid));
    memset(&_reas_buf, 0, sizeof(_reas_buf));
    _reas_active = false;
    _acl_credits = 0;
}

static void bt_handle_event(uint8_t event_code, const uint8_t* payload, size_t len) {
    _wait_debug.last_event_code = event_code;
    _wait_debug.last_event_len = (uint8_t)len;
    switch (event_code) {
    case EVT_CMD_COMPLETE:
        bt_handle_command_complete(payload, len);
        break;
    case EVT_CMD_STATUS:
        bt_handle_command_status(payload, len);
        break;
    case EVT_HARDWARE_ERROR:
        /* the cyw chip reports its post-launch fault through this: the
           1-byte code is the only clue it gives us */
        slog("bluetooth hw_error code=0x%02x\n", len > 0 ? payload[0] : 0xff);
        break;
    case EVT_INQUIRY_COMPLETE:
        _scanning = false;
        bt_emit("scan_done status=%u", len > 0 ? payload[0] : 0xff);
        break;
    case EVT_INQUIRY_RESULT:
        bt_handle_inquiry_result(payload, len);
        break;
    case EVT_INQUIRY_RESULT_RSSI:
        bt_handle_inquiry_result_rssi(payload, len);
        break;
    case EVT_EXTENDED_INQUIRY_RESULT:
        bt_handle_extended_inquiry_result(payload, len);
        break;
    case EVT_REMOTE_NAME_COMPLETE:
        bt_handle_remote_name_complete(payload, len);
        break;
    case EVT_CONN_COMPLETE:
        bt_handle_connection_complete(payload, len);
        break;
    case EVT_DISCONN_COMPLETE:
        bt_handle_disconnection_complete(payload, len);
        break;
    case EVT_AUTH_COMPLETE:
        bt_handle_auth_complete(payload, len);
        break;
    case EVT_PIN_CODE_REQUEST:
        bt_handle_pin_code_request(payload, len);
        break;
    case EVT_LINK_KEY_REQUEST:
        bt_handle_link_key_request(payload, len);
        break;
    case EVT_LINK_KEY_NOTIFY:
        bt_handle_link_key_notify(payload, len);
        break;
    case EVT_CONN_REQUEST:
        bt_handle_conn_request(payload, len);
        break;
    case EVT_IO_CAPABILITY_REQUEST:
        bt_handle_io_capability_request(payload, len);
        break;
    case EVT_USER_CONFIRMATION_REQUEST:
        bt_handle_user_confirmation_request(payload, len);
        break;
    case EVT_USER_PASSKEY_REQUEST:
        bt_handle_user_passkey_request(payload, len);
        break;
    case EVT_SIMPLE_PAIRING_COMPLETE:
        bt_handle_simple_pairing_complete(payload, len);
        break;
    case EVT_NUM_COMPLETED_PKTS:
        bt_handle_num_completed_pkts(payload, len);
        break;
    default:
        break;
    }
}

static int bt_recv_exact(uint8_t* buf, size_t len, uint32_t timeout_ms) {
    size_t i;

    for (i = 0; i < len; ++i) {
        int c = bt_uart_recv_timeout(timeout_ms);
        if (c < 0) {
            return -1;
        }
        buf[i] = (uint8_t)c;
    }
    return 0;
}

static void bt_drop_bytes(size_t len) {
    uint8_t scratch[32];

    while (len > 0) {
        size_t step = len > sizeof(scratch) ? sizeof(scratch) : len;
        if (bt_recv_exact(scratch, step, 10) != 0) {
            return;
        }
        len -= step;
    }
}

static int bt_poll_once(uint32_t first_timeout_ms) {
    int pkt_type;
    uint8_t hdr[4];
    uint8_t payload[MAX_HCI_PAYLOAD];
    uint16_t acl_len;

    pkt_type = bt_uart_recv_timeout(first_timeout_ms);
    if (pkt_type < 0) {
        return 0;
    }

    ++_wait_debug.packets_seen;
    _wait_debug.last_pkt_type = (uint8_t)pkt_type;

    if (pkt_type == HCI_PKT_EVENT) {
        ++_wait_debug.event_packets;
        if (bt_recv_exact(hdr, 2, BT_UART_PKT_FOLLOW_TIMEOUT_MS) != 0) {
            slog("bluetooth poll event_header_timeout lsr=0x%02x msr=0x%02x\n",
                get32(UARTA_LSR_REG) & 0xffu, get32(UARTA_MSR_REG) & 0xffu);
            return -1;
        }
        if (bt_recv_exact(payload, hdr[1], BT_UART_PKT_FOLLOW_TIMEOUT_MS) != 0) {
            slog("bluetooth poll event_payload_timeout evt=0x%02x len=%u lsr=0x%02x msr=0x%02x\n",
                hdr[0], hdr[1], get32(UARTA_LSR_REG) & 0xffu, get32(UARTA_MSR_REG) & 0xffu);
            return -1;
        }
        bt_handle_event(hdr[0], payload, hdr[1]);
        return 1;
    }

    if (pkt_type == HCI_PKT_ACL) {
        uint16_t handle;
        uint8_t pb;

        ++_wait_debug.acl_packets;
        if (bt_recv_exact(hdr, 4, BT_UART_PKT_FOLLOW_TIMEOUT_MS) != 0) {
            slog("bluetooth poll acl_header_timeout lsr=0x%02x msr=0x%02x\n",
                get32(UARTA_LSR_REG) & 0xffu, get32(UARTA_MSR_REG) & 0xffu);
            return -1;
        }
        acl_len = (uint16_t)hdr[2] | ((uint16_t)hdr[3] << 8);
        handle = (uint16_t)(((uint16_t)hdr[0] | ((uint16_t)hdr[1] << 8)) & 0x0fff);
        pb = (uint8_t)((hdr[1] >> 4) & 0x03);

        /* reassemble one L2CAP PDU across pb=0b10 (start) + pb=0b01
           (continuation) fragments; anything else (SCO etc.) is dropped */
        if (pb == 0x02) {
            _reas_handle = handle;
            _reas_active = false;
            _reas_pos = 0;
            _reas_total = 0;
        }
        else if (pb != 0x01 || !_reas_active || handle != _reas_handle) {
            bt_drop_bytes(acl_len);
            return 1;
        }
        if (!_reas_active) {
            /* first fragment of a start packet carries the L2CAP header:
               [len_lo, len_hi, cid_lo, cid_hi] */
            uint8_t l2hdr[4];
            if (acl_len < 4) {
                bt_drop_bytes(acl_len);
                return 1;
            }
            if (bt_recv_exact(l2hdr, 4, BT_UART_PKT_FOLLOW_TIMEOUT_MS) != 0) {
                slog("bluetooth poll acl_l2hdr_timeout lsr=0x%02x msr=0x%02x\n",
                    get32(UARTA_LSR_REG) & 0xffu, get32(UARTA_MSR_REG) & 0xffu);
                return -1;
            }
            acl_len = (uint16_t)(acl_len - 4);
            _reas_total = (uint16_t)((uint16_t)l2hdr[0] | ((uint16_t)l2hdr[1] << 8));
            if (_reas_total > sizeof(_reas_buf)) {
                /* oversized PDU (SDP etc.): swallow the fragments raw */
                bt_drop_bytes(acl_len);
                _reas_handle = 0;
                return 1;
            }
            memcpy(_reas_buf, l2hdr, 4);
            _reas_pos = 4;
            _reas_active = true;
        }
        {
            uint16_t take = acl_len;
            if (_reas_pos + take > 4 + _reas_total) {
                take = (uint16_t)(4 + _reas_total - _reas_pos);
            }
            if (take > 0 &&
                    bt_recv_exact(_reas_buf + _reas_pos, take,
                        BT_UART_PKT_FOLLOW_TIMEOUT_MS) != 0) {
                slog("bluetooth poll acl_payload_timeout lsr=0x%02x msr=0x%02x\n",
                    get32(UARTA_LSR_REG) & 0xffu, get32(UARTA_MSR_REG) & 0xffu);
                return -1;
            }
            if (acl_len > take) {
                bt_drop_bytes((uint16_t)(acl_len - take));
            }
            _reas_pos = (uint16_t)(_reas_pos + take);
        }
        if (_reas_active && _reas_pos >= 4 + _reas_total) {
            _reas_active = false;
            l2cap_dispatch(_reas_handle, _reas_buf, (size_t)(4 + _reas_total));
        }
        return 1;
    }

    ++_wait_debug.other_packets;

    return 1;
}

static int bt_wait_for_opcode(uint16_t opcode, uint32_t timeout_ms) {
    uint32_t waited = 0;

    memset(&_wait_cmd, 0, sizeof(_wait_cmd));
    _wait_cmd.active = true;
    _wait_cmd.opcode = opcode;
    _wait_cmd.status = -1;
    memset(&_wait_debug, 0, sizeof(_wait_debug));
    _wait_debug.last_pkt_type = 0xff;
    _wait_debug.last_event_code = 0xff;
    _wait_debug.last_event_len = 0xff;
    _wait_debug.last_opcode = 0xffff;
    _wait_debug.last_status = -1;

    /* wall-clock bound: a continuous garbage rx stream (wrong baud, floating
       line) keeps bt_poll_once returning > 0, so counting only idle polls
       never reaches the timeout and the daemon hangs */
    {
        uint64_t start_ms = kernel_tic_ms(0);
        while (!_wait_cmd.done && (uint32_t)(kernel_tic_ms(0) - start_ms) < timeout_ms) {
            if (bt_poll_once(1) <= 0) {
                ++waited;
            }
        }
        waited = (uint32_t)(kernel_tic_ms(0) - start_ms);
    }

    _wait_cmd.active = false;
    if (!_wait_cmd.done) {
        slog("bluetooth wait timeout opcode=0x%04x waited=%u seen=%u evt=%u acl=%u other=%u last_pkt=0x%02x last_evt=0x%02x len=%u last_opcode=0x%04x last_status=%d lsr=0x%02x msr=0x%02x\n",
            opcode, waited, _wait_debug.packets_seen, _wait_debug.event_packets,
            _wait_debug.acl_packets, _wait_debug.other_packets,
            _wait_debug.last_pkt_type, _wait_debug.last_event_code,
            _wait_debug.last_event_len, _wait_debug.last_opcode,
            _wait_debug.last_status, get32(UARTA_LSR_REG) & 0xffu,
            get32(UARTA_MSR_REG) & 0xffu);
    }
    return _wait_cmd.done ? _wait_cmd.status : -1;
}

static int bt_hci_command_sync(uint16_t ogf, uint16_t ocf,
        const uint8_t* params, uint8_t param_len, uint32_t timeout_ms) {
    uint16_t opcode = HCI_OPCODE(ogf, ocf);

    if (bt_hci_send_command_raw(opcode, params, param_len) != 0) {
        slog("bluetooth cmd send_failed opcode=0x%04x\n", opcode);
        return -1;
    }
    return bt_wait_for_opcode(opcode, timeout_ms);
}

static int bt_load_firmware(void) {
    uint32_t offset = 0;
    uint32_t chunk_idx = 0;
    uint8_t opcodebytes[2];
    uint8_t length;
    const uint8_t* data = bcm4345c0_hcd;
    uint32_t size = bcm4345c0_hcd_len;
    int ret;

    slog("bluetooth fw blob size=%u head=%02x %02x %02x %02x %02x %02x %02x %02x\n",
        size,
        size > 0 ? data[0] : 0,
        size > 1 ? data[1] : 0,
        size > 2 ? data[2] : 0,
        size > 3 ? data[3] : 0,
        size > 4 ? data[4] : 0,
        size > 5 ? data[5] : 0,
        size > 6 ? data[6] : 0,
        size > 7 ? data[7] : 0);

    ret = bt_hci_command_sync(HCI_OGF_VENDOR, HCI_OCF_VENDOR_LOAD_FIRMWARE, NULL, 0, 1000);
    if (ret != 0) {
        slog("bluetooth fw enter_download_failed ret=%d\n", ret);
        return ret;
    }

    while (offset < size) {
        uint16_t opcode;

        opcodebytes[0] = *data++;
        opcodebytes[1] = *data++;
        length = *data++;
        opcode = (uint16_t)opcodebytes[0] | ((uint16_t)opcodebytes[1] << 8);
        if (chunk_idx == 0) {
            slog("bluetooth fw first_chunk opcode=0x%04x len=%u\n", opcode, length);
        }

        ret = bt_hci_send_command_raw(opcode, data, length);
        if (ret != 0) {
            slog("bluetooth fw send_chunk_failed idx=%u offset=%u opcode=0x%04x len=%u ret=%d\n",
                chunk_idx, offset, opcode, length, ret);
            return ret;
        }
        ret = bt_wait_for_opcode(opcode, 1000);
        if (ret != 0) {
            slog("bluetooth fw wait_chunk_failed idx=%u offset=%u opcode=0x%04x len=%u ret=%d\n",
                chunk_idx, offset, opcode, length, ret);
            return ret;
        }

        data += length;
        offset += (uint32_t)length + 3;
        ++chunk_idx;
    }

    return 0;
}

static int bt_configure_controller(void) {
    uint8_t mask[8] = {0xff, 0xff, 0xfb, 0xff, 0x07, 0xf8, 0xbf, 0x3d};
    uint8_t scan_enable = 0x03;
    uint8_t auth_enable = 0x01;
    uint8_t inquiry_mode = 0x02;
    uint8_t simple_pair = 0x01;
    uint8_t buf_ret[8];
    uint8_t buf_ret_len = 0;

    if (bt_hci_command_sync(HCI_OGF_HOST_CTRL, HCI_OCF_RESET, NULL, 0, 1000) != 0) {
        return -1;
    }
    (void)bt_hci_command_sync(HCI_OGF_HOST_CTRL, HCI_OCF_SET_EVENT_MASK, mask, sizeof(mask), 1000);
    /* READ_BUFFER_SIZE (OGF_INFO 0x04, ret: status already stripped ->
       [acl_pkt_len_lo, acl_pkt_len_hi, sco_len, acl_num, sco_num]):
       acl_num is our host->controller ACL credit pool */
    if (bt_hci_command_sync_ret(HCI_OGF_INFO, HCI_OCF_READ_BUFFER_SIZE, NULL, 0,
            1000, buf_ret, sizeof(buf_ret), &buf_ret_len) == 0 && buf_ret_len >= 5) {
        _acl_credits = (uint16_t)buf_ret[3] | ((uint16_t)buf_ret[4] << 8);
    }
    if (_acl_credits == 0) {
        _acl_credits = 8; /* sane fallback if the controller stays mute */
    }
    slog("bluetooth acl_credits=%u\n", _acl_credits);
    (void)bt_hci_command_sync(HCI_OGF_HOST_CTRL, HCI_OCF_WRITE_AUTH_ENABLE, &auth_enable, 1, 1000);
    (void)bt_hci_command_sync(HCI_OGF_HOST_CTRL, HCI_OCF_WRITE_INQUIRY_MODE, &inquiry_mode, 1, 1000);
    (void)bt_hci_command_sync(HCI_OGF_HOST_CTRL, HCI_OCF_WRITE_SIMPLE_PAIRING_MODE, &simple_pair, 1, 1000);
    (void)bt_hci_command_sync(HCI_OGF_HOST_CTRL, HCI_OCF_WRITE_SCAN_ENABLE, &scan_enable, 1, 1000);
    return 0;
}

static int bt_driver_init(void) {
    int ret;
    int flushed;
    int attempt;

    _mmio_base = mmio_map();
    if (_mmio_base == 0) {
        slog("bluetooth init mmio_map_failed\n");
        return -1;
    }

    bt_prepare_combo_chip_power();
    bt_release_bt_shutdown();

    pi5_bt_uart_init();
    flushed = bt_uart_flush();
    slog("bluetooth init 16550 clock=%u div=%u flushed=%d lsr=0x%02x msr=0x%02x\n",
        PI5_BT_UART_CLOCK_HZ, PI5_BT_UART_CLOCK_HZ / (16u * PI5_BT_BAUD_RATE),
        flushed, get32(UARTA_LSR_REG) & 0xffu, get32(UARTA_MSR_REG) & 0xffu);
    usleep(1000000);

    ret = bt_hci_command_sync(HCI_OGF_HOST_CTRL, HCI_OCF_RESET, NULL, 0, 3000);
    if (ret != 0) {
        slog("bluetooth init reset_chip_failed ret=%d\n", ret);
        return ret;
    }
    usleep(1000000);

    ret = bt_load_firmware();
    if (ret != 0) {
        slog("bluetooth init load_firmware_failed ret=%d\n", ret);
        return ret;
    }
    usleep(300000);

    /* the patchram launch record reboots the chip into RAM firmware; the
       uart glitches during the handoff (stray 0x00 bytes) and the fresh
       firmware may need a few hundred ms before answering - flush the
       garbage and retry the post-fw reset instead of failing once */
    ret = -1;
    for (attempt = 0; attempt < 5 && ret != 0; ++attempt) {
        usleep(200000);
        bt_uart_flush();
        ret = bt_hci_command_sync(HCI_OGF_HOST_CTRL, HCI_OCF_RESET, NULL, 0, 1000);
        if (ret != 0) {
            slog("bluetooth init post_fw_reset retry=%d ret=%d\n", attempt, ret);
        }
    }
    if (ret != 0) {
        slog("bluetooth init post_fw_reset_failed\n");
        /* is anything still alive out there? READ_LOCAL_VERSION is legal
           both in rom download mode and in the patchram firmware */
        if (bt_hci_command_sync(HCI_OGF_INFO, HCI_OCF_READ_LOCAL_VERSION, NULL, 0, 500) == 0) {
            slog("bluetooth init post_fw_probe answered (chip alive, fw state wrong)\n");
        }
        else {
            slog("bluetooth init post_fw_probe silent (chip wedged)\n");
        }
        return ret;
    }

    ret = bt_configure_controller();
    if (ret != 0) {
        slog("bluetooth init configure_controller_failed ret=%d\n", ret);
        return ret;
    }

    _ready = true;
    return 0;
}

static int bt_start_scan(int seconds) {
    uint8_t params[5];
    int inquiry_len;
    int ret;

    if (!_ready) {
        return -1;
    }

    if (seconds <= 0) {
        seconds = 10;
    }
    inquiry_len = (seconds * 100 + 127) / 128;
    if (inquiry_len < 1) {
        inquiry_len = 1;
    }
    if (inquiry_len > 0x30) {
        inquiry_len = 0x30;
    }

    params[0] = 0x33;
    params[1] = 0x8b;
    params[2] = 0x9e;
    params[3] = (uint8_t)inquiry_len;
    params[4] = 0x00;

    _scanning = true;
    ret = bt_hci_command_sync(HCI_OGF_LINK_CTRL, HCI_OCF_INQUIRY, params, sizeof(params), 1500);
    if (ret != 0) {
        _scanning = false;
        return ret;
    }
    return 0;
}

static int bt_stop_scan(void) {
    int ret = bt_hci_command_sync(HCI_OGF_LINK_CTRL, HCI_OCF_INQUIRY_CANCEL, NULL, 0, 1000);

    _scanning = false;
    return ret;
}

static int bt_start_connection(const uint8_t* addr, bool pair, const char* pin,
        char* ret_text, size_t ret_text_sz) {
    bt_device_t* dev = bt_find_device(addr, true);
    char addr_str[24];
    int ret;
    const char* action = pair ? "pair" : "connect";

    if (dev == NULL) {
        bt_addr_to_str(addr, addr_str, sizeof(addr_str));
        if (ret_text != NULL && ret_text_sz != 0) {
            snprintf(ret_text, ret_text_sz, "%s_fail %s reason=unknown_device\n", action, addr_str);
        }
        return -1;
    }

    bt_addr_to_str(addr, addr_str, sizeof(addr_str));
    if (dev->connected) {
        if (pair) {
            memset(&_pending, 0, sizeof(_pending));
            _pending.type = BT_PENDING_PAIR;
            memcpy(_pending.addr, addr, 6);
            _pending.handle = dev->handle;
            strncpy(_pending.pin, (pin != NULL && pin[0] != 0) ? pin : "0000", sizeof(_pending.pin) - 1);
            bt_hci_auth_request(dev->handle);
            if (ret_text != NULL && ret_text_sz != 0) {
                snprintf(ret_text, ret_text_sz, "pair_wait_auth %s\n", addr_str);
            }
            return 0;
        }
        if (ret_text != NULL && ret_text_sz != 0) {
            snprintf(ret_text, ret_text_sz, "connect_ok %s handle=0x%04X\n", addr_str, dev->handle);
        }
        return 0;
    }

    memset(&_pending, 0, sizeof(_pending));
    _pending.type = pair ? BT_PENDING_PAIR : BT_PENDING_CONNECT;
    memcpy(_pending.addr, addr, 6);
    strncpy(_pending.pin, (pin != NULL && pin[0] != 0) ? pin : "0000", sizeof(_pending.pin) - 1);

    ret = bt_hci_create_connection(dev);
    if (ret != 0) {
        bt_clear_pending();
        if (ret_text != NULL && ret_text_sz != 0) {
            snprintf(ret_text, ret_text_sz, "%s_fail %s status=%d\n", action, addr_str, ret);
        }
        return ret;
    }

    ret = bt_wait_for_opcode(HCI_OPCODE(HCI_OGF_LINK_CTRL, HCI_OCF_CREATE_CONN), 1500);
    if (ret != 0) {
        bt_clear_pending();
        if (ret_text != NULL && ret_text_sz != 0) {
            snprintf(ret_text, ret_text_sz, "%s_fail %s status=%d\n", action, addr_str, ret);
        }
        return ret;
    }

    if (ret_text != NULL && ret_text_sz != 0) {
        snprintf(ret_text, ret_text_sz, "%s_begin %s", action, addr_str);
    }
    return 0;
}

/* walk the persistent store and page every device we connected with
   before; each attempt is bounded (1.5s cmd status + 3s page wait) so a
   drawer full of absent devices cannot stall the daemon for long */
static void bt_autoconnect_known(void) {
    int i;

    if (!_ready || _scanning || _pending.type != BT_PENDING_NONE) {
        return;
    }

    for (i = 0; i < MAX_BT_KNOWN; ++i) {
        bt_device_t* dev;
        char addr_str[24];
        uint64_t start_ms;

        if (!_known[i].used) {
            continue;
        }
        dev = bt_find_device(_known[i].addr, true);
        if (dev == NULL || dev->connected) {
            continue;
        }

        bt_addr_to_str(_known[i].addr, addr_str, sizeof(addr_str));
        bt_emit("auto_connect %s\n", addr_str);
        slog("bluetooth auto_connect %s\n", addr_str);
        if (bt_hci_create_connection(dev) != 0) {
            continue;
        }
        if (bt_wait_for_opcode(HCI_OPCODE(HCI_OGF_LINK_CTRL, HCI_OCF_CREATE_CONN), 1500) != 0) {
            continue;
        }
        /* the conn-complete event arrives asynchronously; poll it out */
        start_ms = kernel_tic_ms(0);
        while (!dev->connected &&
                (uint32_t)(kernel_tic_ms(0) - start_ms) < 3000) {
            bt_poll_once(10);
        }
    }
}

static void bt_list_devices_ret(char* ret, size_t ret_sz) {
    int i;
    int count = 0;

    for (i = 0; i < MAX_BT_DEVICES; ++i) {
        if (!_devices[i].used) {
            continue;
        }
        bt_ret_append_device_line(i, ret, ret_sz, "device", &_devices[i]);
        ++count;
    }
    bt_ret_append(ret, ret_sz, "devices_done count=%d\n", count);
}

static void bt_list_known_ret(char* ret, size_t ret_sz) {
    int i;
    int count = 0;

    for (i = 0; i < MAX_BT_KNOWN; ++i) {
        char addr[24];

        if (!_known[i].used) {
            continue;
        }
        bt_addr_to_str(_known[i].addr, addr, sizeof(addr));
        bt_ret_append(ret, ret_sz, "%d: known %s paired=%d key=%d name=%s\n",
            i,
            addr,
            _known[i].paired ? 1 : 0,
            _known[i].has_key ? 1 : 0,
            _known[i].name[0] ? _known[i].name : "-");
        ++count;
    }
    bt_ret_append(ret, ret_sz, "known_done count=%d\n", count);
}

static int bt_forget_device(const char* arg, char* ret, size_t ret_sz) {
    uint8_t addr[6];
    bt_known_t* k;
    bt_device_t* dev;

    if (!bt_parse_addr(arg, addr)) {
        if (ret != NULL && ret_sz != 0) {
            snprintf(ret, ret_sz, "forget_fail reason=bad_addr\n");
        }
        return -1;
    }

    k = bt_known_find(addr);
    if (k == NULL) {
        if (ret != NULL && ret_sz != 0) {
            snprintf(ret, ret_sz, "forget_fail %s reason=not_found\n", arg);
        }
        return -1;
    }

    k->used = false;
    /* drop the cached link key too, or the next connect would silently
       reuse the very credentials the user just asked to forget */
    dev = bt_find_device(addr, false);
    if (dev != NULL) {
        dev->has_link_key = false;
        memset(dev->link_key, 0, sizeof(dev->link_key));
    }
    bt_known_save();
    if (ret != NULL && ret_sz != 0) {
        snprintf(ret, ret_sz, "forget_ok %s\n", arg);
    }
    return 0;
}

static void bt_dump_state_ret(char* ret, size_t ret_sz) {
    int i;
    int count = 0;

    for (i = 0; i < MAX_BT_DEVICES; ++i) {
        if (_devices[i].used) {
            ++count;
        }
    }
    bt_ret_append(ret, ret_sz, "state powered=%d ready=%d scanning=%d devices=%d pending=%d\n",
        _powered ? 1 : 0,
        _ready ? 1 : 0,
        _scanning ? 1 : 0,
        count,
        (int)_pending.type);
}

static void bt_help_emit(void) {
    bt_emit("open\n");
    bt_emit("close\n");
    bt_emit("scan [seconds]\n");
    bt_emit("stop\n");
    bt_emit("devices\n");
    bt_emit("known\n");
    bt_emit("forget <bdaddr>\n");
    bt_emit("state\n");
    bt_emit("name <bdaddr>\n");
    bt_emit("connect <bdaddr>\n");
    bt_emit("pair <bdaddr> [pin]\n");
    bt_emit("disconnect <bdaddr|handle>\n");
    bt_emit("hid_open <bdaddr>\n");
    bt_emit("hid_close\n");
    bt_emit("hid_state\n");
}

static void bt_help_ret(char* ret, size_t ret_sz) {
    bt_ret_append(ret, ret_sz, "open: power on the bluetooth adapter\n");
    bt_ret_append(ret, ret_sz, "close: power off the bluetooth adapter\n");
    bt_ret_append(ret, ret_sz, "scan [seconds]\n");
    bt_ret_append(ret, ret_sz, "stop\n");
    bt_ret_append(ret, ret_sz, "devices\n");
    bt_ret_append(ret, ret_sz, "known: list devices remembered in /etc/bt/bt.json\n");
    bt_ret_append(ret, ret_sz, "forget <bdaddr>: drop a remembered device\n");
    bt_ret_append(ret, ret_sz, "state\n");
    bt_ret_append(ret, ret_sz, "name <bdaddr>\n");
    bt_ret_append(ret, ret_sz, "connect <bdaddr>\n");
    bt_ret_append(ret, ret_sz, "pair <bdaddr> [pin]\n");
    bt_ret_append(ret, ret_sz, "disconnect <bdaddr|handle>\n");
    bt_ret_append(ret, ret_sz, "hid_open <bdaddr>: open HID ctrl+intr channels on a connected device\n");
    bt_ret_append(ret, ret_sz, "hid_close: tear the HID session down\n");
    bt_ret_append(ret, ret_sz, "hid_state: HID session + subscriber summary\n");
}

static int bt_name_request(const uint8_t* addr, char* ret_text, size_t ret_text_sz) {
    bt_device_t* dev = bt_find_device(addr, false);
    char addr_str[24];
    int ret;

    bt_addr_to_str(addr, addr_str, sizeof(addr_str));
    if (dev == NULL) {
        if (ret_text != NULL && ret_text_sz != 0) {
            snprintf(ret_text, ret_text_sz, "name_fail %s reason=unknown_device\n", addr_str);
        }
        return -1;
    }

    ret = bt_hci_request_remote_name(dev);
    if (ret != 0) {
        if (ret_text != NULL && ret_text_sz != 0) {
            snprintf(ret_text, ret_text_sz, "name_fail %s status=%d\n", addr_str, ret);
        }
        return ret;
    }
    ret = bt_wait_for_opcode(HCI_OPCODE(HCI_OGF_LINK_CTRL, HCI_OCF_REMOTE_NAME_REQ), 1500);
    if (ret != 0) {
        if (ret_text != NULL && ret_text_sz != 0) {
            snprintf(ret_text, ret_text_sz, "name_fail %s status=%d\n", addr_str, ret);
        }
        return ret;
    }
    if (ret_text != NULL && ret_text_sz != 0) {
        snprintf(ret_text, ret_text_sz, "name_begin %s\n", addr_str);
    }
    return 0;
}

static int bt_disconnect_target(const char* arg, char* ret_text, size_t ret_text_sz) {
    uint8_t addr[6];
    bt_device_t* dev = NULL;
    uint16_t handle;
    char* endptr;
    unsigned long value;

    if (bt_parse_addr(arg, addr)) {
        dev = bt_find_device(addr, false);
        if (dev == NULL || !dev->connected) {
            if (ret_text != NULL && ret_text_sz != 0) {
                snprintf(ret_text, ret_text_sz, "disconnect_fail %s reason=not_connected\n", arg);
            }
            return -1;
        }
        handle = dev->handle;
    }
    else {
        value = strtoul(arg, &endptr, 0);
        if (*arg == 0 || *endptr != 0 || value > 0xffffUL) {
            if (ret_text != NULL && ret_text_sz != 0) {
                snprintf(ret_text, ret_text_sz, "disconnect_fail %s reason=bad_arg\n", arg);
            }
            return -1;
        }
        handle = (uint16_t)value;
    }

    bt_hci_disconnect(handle);
    if (bt_wait_for_opcode(HCI_OPCODE(HCI_OGF_LINK_CTRL, HCI_OCF_DISCONNECT), 1500) != 0) {
        if (ret_text != NULL && ret_text_sz != 0) {
            snprintf(ret_text, ret_text_sz, "disconnect_fail handle=0x%04X reason=cmd_status\n", handle);
        }
        return -1;
    }
    if (ret_text != NULL && ret_text_sz != 0) {
        snprintf(ret_text, ret_text_sz, "disconnect_begin handle=0x%04X\n", handle);
    }
    return 0;
}

static void bt_disconnect_all(void) {
    int i;

    for (i = 0; i < MAX_BT_DEVICES; ++i) {
        if (_devices[i].used && _devices[i].connected) {
            bt_hci_disconnect(_devices[i].handle);
        }
    }
}

static void bt_mark_all_disconnected(void) {
    int i;

    for (i = 0; i < MAX_BT_DEVICES; ++i) {
        if (_devices[i].used) {
            _devices[i].connected = false;
            _devices[i].handle = 0;
        }
    }
    bt_hid_stack_reset();
}

/* "open": bring the radio up. A cold open power-cycles BT_ON and reloads the
   firmware via bt_driver_init(); a warm open just re-enables scan so the
   adapter is discoverable + connectable again. */
static int bt_open_adapter(char* ret, size_t ret_sz) {
    uint8_t scan_enable = 0x03;
    int r;

    if (_ready && _powered) {
        r = bt_hci_command_sync(HCI_OGF_HOST_CTRL, HCI_OCF_WRITE_SCAN_ENABLE,
                &scan_enable, 1, 1000);
        bt_emit("power_on state=already_on\n");
        bt_autoconnect_known();
        if (ret != NULL && ret_sz != 0) {
            snprintf(ret, ret_sz, "open_ok state=already_on scan=%d\n", r == 0 ? 1 : 0);
        }
        return 0;
    }

    r = bt_driver_init();
    if (r != 0) {
        if (ret != NULL && ret_sz != 0) {
            snprintf(ret, ret_sz, "open_fail reason=init status=%d\n", r);
        }
        return r;
    }
    _powered = true;
    bt_emit("power_on state=powered_on\n");
    bt_autoconnect_known();
    if (ret != NULL && ret_sz != 0) {
        snprintf(ret, ret_sz, "open_ok state=powered_on\n");
    }
    return 0;
}

/* "close": drop every link, go non-discoverable, then pull BT_ON low so the
   BT core powers down. WL_ON is left untouched so a co-resident wifi chip on
   the same combo module keeps running. */
static int bt_close_adapter(char* ret, size_t ret_sz) {
    uint8_t scan_enable = 0x00;

    if (!_powered) {
        _ready = false;
        _scanning = false;
        bt_clear_pending();
        bt_emit("power_off state=already_off\n");
        if (ret != NULL && ret_sz != 0) {
            snprintf(ret, ret_sz, "close_ok state=already_off\n");
        }
        return 0;
    }

    if (_scanning) {
        (void)bt_stop_scan();
    }
    bt_disconnect_all();
    if (_ready) {
        (void)bt_hci_command_sync(HCI_OGF_HOST_CTRL, HCI_OCF_WRITE_SCAN_ENABLE,
                &scan_enable, 1, 500);
    }

    /* gio29 is BT_ON on the Pi 5 wifi/bt combo chip. */
    pi5_gio_output(BT_ON_BIT, false);
    usleep(100000);

    _powered = false;
    _ready = false;
    _scanning = false;
    bt_clear_pending();
    bt_mark_all_disconnected();
    bt_emit("power_off state=powered_off\n");
    if (ret != NULL && ret_sz != 0) {
        snprintf(ret, ret_sz, "close_ok state=powered_off\n");
    }
    return 0;
}

static int bt_handle_cmd_args(int argc, char** argv, char* ret, size_t ret_sz) {
    const char* cmd;
    const char* arg1;
    const char* arg2;
    uint8_t addr[6];

    if (ret_sz == 0) {
        return -1;
    }
    ret[0] = 0;

    if (argc <= 0 || argv == NULL || argv[0] == NULL) {
        snprintf(ret, ret_sz, "missing command\n");
        return -1;
    }

    cmd = argv[0];
    arg1 = argc > 1 ? argv[1] : NULL;
    arg2 = argc > 2 ? argv[2] : NULL;

    if (strcmp(cmd, "help") == 0) {
        bt_help_ret(ret, ret_sz);
    }
    else if (strcmp(cmd, "open") == 0 || strcmp(cmd, "on") == 0 ||
            strcmp(cmd, "poweron") == 0) {
        bt_open_adapter(ret, ret_sz);
    }
    else if (strcmp(cmd, "close") == 0 || strcmp(cmd, "off") == 0 ||
            strcmp(cmd, "poweroff") == 0) {
        bt_close_adapter(ret, ret_sz);
    }
    else if (strcmp(cmd, "state") == 0) {
        bt_dump_state_ret(ret, ret_sz);
    }
    else if (strcmp(cmd, "devices") == 0) {
        bt_list_devices_ret(ret, ret_sz);
    }
    else if (strcmp(cmd, "known") == 0) {
        bt_list_known_ret(ret, ret_sz);
    }
    else if (strcmp(cmd, "forget") == 0) {
        if (arg1 == NULL) {
            snprintf(ret, ret_sz, "forget_fail reason=missing_target\n");
            return 0;
        }
        bt_forget_device(arg1, ret, ret_sz);
    }
    else if (strcmp(cmd, "scan") == 0) {
        int seconds = arg1 != NULL ? atoi(arg1) : 10;
        if (!_ready) {
            snprintf(ret, ret_sz, "scan_fail reason=not_ready\n");
            return 0;
        }
        if (_scanning) {
            snprintf(ret, ret_sz, "scan_busy\n");
            return 0;
        }
        int scan_ret = bt_start_scan(seconds);
        if (scan_ret != 0) {
            snprintf(ret, ret_sz, "scan_fail status=%d\n", scan_ret);
            return 0;
        }
        snprintf(ret, ret_sz, "scan_begin seconds=%d\n", seconds > 0 ? seconds : 10);
    }
    else if (strcmp(cmd, "stop") == 0) {
        int stop_ret = bt_stop_scan();
        snprintf(ret, ret_sz, "scan_stop status=%d\n", stop_ret);
    }
    else if (strcmp(cmd, "name") == 0) {
        if (!_ready) {
            snprintf(ret, ret_sz, "name_fail reason=not_ready\n");
            return 0;
        }
        if (arg1 == NULL || !bt_parse_addr(arg1, addr)) {
            snprintf(ret, ret_sz, "name_fail reason=bad_addr\n");
            return 0;
        }
        bt_name_request(addr, ret, ret_sz);
    }
    else if (strcmp(cmd, "connect") == 0) {
        if (!_ready) {
            snprintf(ret, ret_sz, "connect_fail reason=not_ready\n");
            return 0;
        }
        if (arg1 == NULL || !bt_parse_addr(arg1, addr)) {
            snprintf(ret, ret_sz, "connect_fail reason=bad_addr\n");
            return 0;
        }
        bt_start_connection(addr, false, NULL, ret, ret_sz);
    }
    else if (strcmp(cmd, "pair") == 0) {
        if (!_ready) {
            snprintf(ret, ret_sz, "pair_fail reason=not_ready\n");
            return 0;
        }
        if (arg1 == NULL || !bt_parse_addr(arg1, addr)) {
            snprintf(ret, ret_sz, "pair_fail reason=bad_addr\n");
            return 0;
        }
        bt_start_connection(addr, true, arg2, ret, ret_sz);
    }
    else if (strcmp(cmd, "disconnect") == 0) {
        if (!_ready) {
            snprintf(ret, ret_sz, "disconnect_fail reason=not_ready\n");
            return 0;
        }
        if (arg1 == NULL) {
            snprintf(ret, ret_sz, "disconnect_fail reason=missing_target\n");
            return 0;
        }
        bt_disconnect_target(arg1, ret, ret_sz);
    }
    else if (strcmp(cmd, "hid_open") == 0) {
        bt_device_t* dev;

        if (!_ready) {
            snprintf(ret, ret_sz, "hid_open_fail reason=not_ready\n");
            return 0;
        }
        if (arg1 == NULL || !bt_parse_addr(arg1, addr)) {
            snprintf(ret, ret_sz, "hid_open_fail reason=bad_addr\n");
            return 0;
        }
        dev = bt_find_device(addr, false);
        if (dev == NULL || !dev->connected || dev->handle == 0) {
            snprintf(ret, ret_sz, "hid_open_fail reason=not_connected\n");
            return 0;
        }
        bt_hid_start(dev->handle, dev->addr);
        snprintf(ret, ret_sz, "hid_open_begin handle=0x%04X\n", dev->handle);
    }
    else if (strcmp(cmd, "hid_close") == 0) {
        if (!_hid.active) {
            snprintf(ret, ret_sz, "hid_close_fail reason=no_session\n");
            return 0;
        }
        bt_hid_stop();
        snprintf(ret, ret_sz, "hid_close_ok\n");
    }
    else if (strcmp(cmd, "hid_state") == 0) {
        char haddr[24];
        int sub_cnt = 0;
        int i;

        for (i = 0; i < BT_SUBSCRIBER_MAX; ++i) {
            if (_subs[i].used && _subs[i].report_id == BT_REPORT_ID_MOUSE) {
                ++sub_cnt;
            }
        }
        if (!_hid.active) {
            snprintf(ret, ret_sz, "hid_state active=0 mouse_subs=%d\n", sub_cnt);
            return 0;
        }
        bt_addr_to_str(_hid.addr, haddr, sizeof(haddr));
        snprintf(ret, ret_sz,
            "hid_state active=1 addr=%s handle=0x%04X up=%d boot=%d "
            "ctrl=%d intr=%d mouse_subs=%d\n",
            haddr, _hid.acl_handle, _hid.up ? 1 : 0, _hid.boot_protocol_ok ? 1 : 0,
            _hid.ctrl != NULL ? _hid.ctrl->state : -1,
            _hid.intr != NULL ? _hid.intr->state : -1,
            sub_cnt);
    }
    else {
        snprintf(ret, ret_sz, "unknown command\n");
        return 0;
    }
    return 0;
}

static int bt_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        void* buf, int size, int offset, void* p) {
    bt_subscriber_t* sub;
    int i;

    (void)dev;
    (void)node;
    (void)offset;
    (void)p;

    if (size <= 0) {
        return VFS_ERR_RETRY;
    }

    /* a subscriber that picked a report id reads fixed events from its
       own queue (usbhidsrv wire protocol); report_id 0 keeps the legacy
       text event stream so xbt/devcmd readers are unaffected */
    sub = bt_sub_find(fd, from_pid);
    if (sub != NULL && sub->report_id != 0) {
        int n = sub_queue_pop(sub, buf, size);
        return n > 0 ? n : VFS_ERR_RETRY;
    }

    for (i = 0; i < size; ++i) {
        if (charbuf_pop(_evt_buf, ((char*)buf) + i) != 0) {
            break;
        }
    }
    return i == 0 ? VFS_ERR_RETRY : i;
}

static char* bt_dev_cmd(vdevice_t* dev, int from_pid, int argc, char** argv, void* p) {
    char* ret = (char*)malloc(BT_CMD_RET_SZ);
    if (ret == NULL) {
        return NULL;
    }
    memset(ret, 0, BT_CMD_RET_SZ);

    (void)dev;
    (void)from_pid;
    (void)p;

    if (bt_handle_cmd_args(argc, argv, ret, BT_CMD_RET_SZ) != 0) {
        return ret;
    }
    if (ret[0] == 0) {
        snprintf(ret, BT_CMD_RET_SZ, "ok");
    }
    return ret;
}

static uint32_t bt_check_poll_events(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node, void* p) {
    bt_subscriber_t* sub;

    (void)dev;
    (void)node;
    (void)p;

    sub = bt_sub_find(fd, from_pid);
    if (sub != NULL && sub->report_id != 0) {
        return sub_queue_has_data(sub) ? VFS_EVT_RD : 0;
    }
    if (_evt_buf != NULL && !charbuf_is_empty(_evt_buf)) {
        return VFS_EVT_RD;
    }
    return 0;
}

static int bt_loop(vdevice_t* dev, void* p) {
    int packets = 0;

    (void)dev;
    (void)p;

    while (bt_poll_once(0) > 0) {
        ++packets;
    }

    /* L2CAP channel state machine (connect/config retries, timeouts,
       SET_PROTOCOL kick once both HID channels are open) */
    l2cap_step();

    /* a mouse subscriber's edge wake can get spent on a generic IPC wait:
       re-assert while queues still hold undrained events */
    if (_sub_reassert_ms != 0 && (int64_t)(kernel_tic_ms(0) - _sub_reassert_ms) >= 0) {
        if (bt_hid_backlog()) {
            bt_hid_rewake_backlog();
            _sub_reassert_ms = kernel_tic_ms(0) + BT_HID_REASSERT_MS;
        }
        else {
            _sub_reassert_ms = 0;
        }
    }

    if (packets == 0) {
        proc_usleep(_idle_sleep_us);
        if (_idle_sleep_us < 50000) {
            _idle_sleep_us <<= 1;
        }
    }
    else {
        _idle_sleep_us = 1000;
    }
    return 0;
}

int main(int argc, char** argv) {
    vdevice_t dev;
    const char* mnt_point = argc > 1 ? argv[1] : "/dev/bt0";

    _evt_buf = charbuf_new(0);
    if (_evt_buf == NULL) {
        return -1;
    }

    memset(&dev, 0, sizeof(dev));
    strcpy(dev.desc, "bluetooth");
    dev.open = bt_vdev_open;
    dev.close = bt_vdev_close;
    dev.fcntl = bt_vdev_fcntl;
    dev.read = bt_read;
    dev.loop_step = bt_loop;
    dev.check_poll_events = bt_check_poll_events;
    dev.cmd = bt_dev_cmd;
    _bt_dev = &dev;

    bt_known_load();
    bt_known_seed_devices();
    /* materialize the store right away (even while still empty) so its
       presence - and the SD card's writability - is visible at boot
       instead of only after the first successful connect */
    if (access(BT_KNOWN_FILE, F_OK) != 0) {
        bt_known_save();
    }

    if (bt_driver_init() != 0) {
        slog("bluetooth error init_failed\n");
    }
    else {
        _powered = true;
        slog("bluetooth ready classic_hci=1 scan=1 pair=1 connect=1\n");
        bt_help_emit();
        bt_autoconnect_known();
    }

    device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666, false);

    charbuf_free(_evt_buf);
    return 0;
}
