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

/* the report-descriptor parser and the /dev/bt0 subscriber fan-out are
   shared with usbhostd, so a report decoded over HIDP (classic) or HOGP
   (GATT) produces the identical bytes for hid_keybd/hid_moused */
#include <hid/hid_defs.h>
#include <hid/hid_report.h>
#include <hid/hid_srv.h>

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
#define HCI_OGF_LE 0x08
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

/* LE controller commands (OGF 0x08) */
#define HCI_OCF_LE_SET_EVENT_MASK 0x0001
#define HCI_OCF_LE_READ_BUFFER_SIZE 0x0002
#define HCI_OCF_LE_SET_RANDOM_ADDRESS 0x0005
#define HCI_OCF_LE_SET_SCAN_PARAMS 0x000b
#define HCI_OCF_LE_SET_SCAN_ENABLE 0x000c
#define HCI_OCF_LE_CREATE_CONNECTION 0x000d
#define HCI_OCF_LE_CREATE_CONN_CANCEL 0x000e
#define HCI_OCF_LE_RAND 0x0018
#define HCI_OCF_LE_START_ENCRYPTION 0x0019
#define HCI_OCF_LE_LTK_REQ_REPLY 0x001a
#define HCI_OCF_LE_LTK_REQ_NEG_REPLY 0x001b

/* host-controller commands we need for the LE path (OGF 0x03) */
#define HCI_OCF_READ_BD_ADDR 0x0009

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
#define EVT_ENCRYPTION_CHANGE 0x08
/* every LE controller event is wrapped in this one; the first payload
   byte is the sub-event code */
#define EVT_LE_META 0x3e

/* LE meta sub-event codes */
#define LE_EVT_CONN_COMPLETE 0x01
#define LE_EVT_ADV_REPORT 0x02
#define LE_EVT_CONN_UPDATE 0x03
#define LE_EVT_LTK_REQUEST 0x05
#define LE_EVT_REMOTE_FEATURES 0x04
#define LE_EVT_ENHANCED_CONN_COMPLETE 0x0a
#define LE_EVT_EXT_ADV_REPORT 0x0d

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

/*
 * Bluetooth LE hosts a different L2CAP flavour: the HID traffic does not
 * ride a PSM but the ATT fixed channel, and pairing rides the SMP fixed
 * channel. Both exist as soon as the LE link is up - there is no
 * connection/configuration handshake to run, and the peer's MTU is the
 * spec default until ATT Exchange MTU says otherwise.
 */
#define L2CAP_CID_ATT 0x0004
#define L2CAP_CID_LE_SIGNAL 0x0005
#define L2CAP_CID_SMP 0x0006

#define L2CAP_SIG_LE_CONN_PARAM_UPDATE_REQ 0x12
#define L2CAP_SIG_LE_CONN_PARAM_UPDATE_RSP 0x13

#define L2CAP_LE_MTU_DEFAULT 23
#define L2CAP_LE_MPS_DEFAULT 23

/* ATT opcodes (Vol 3 Part F 3.4) */
#define ATT_OP_ERROR_RSP 0x01
#define ATT_OP_MTU_REQ 0x02
#define ATT_OP_MTU_RSP 0x03
#define ATT_OP_FIND_INFO_REQ 0x04
#define ATT_OP_FIND_INFO_RSP 0x05
#define ATT_OP_FIND_BY_TYPE_VALUE_REQ 0x06
#define ATT_OP_FIND_BY_TYPE_VALUE_RSP 0x07
#define ATT_OP_READ_BY_TYPE_REQ 0x08
#define ATT_OP_READ_BY_TYPE_RSP 0x09
#define ATT_OP_READ_REQ 0x0a
#define ATT_OP_READ_RSP 0x0b
#define ATT_OP_READ_BLOB_REQ 0x0c
#define ATT_OP_READ_BLOB_RSP 0x0d
#define ATT_OP_READ_BY_GROUP_REQ 0x10
#define ATT_OP_READ_BY_GROUP_RSP 0x11
#define ATT_OP_WRITE_REQ 0x12
#define ATT_OP_WRITE_RSP 0x13
#define ATT_OP_HANDLE_NOTIFY 0x1b
#define ATT_OP_HANDLE_IND 0x1d
#define ATT_OP_IND_CONFIRM 0x1e
#define ATT_OP_WRITE_CMD 0x52

#define ATT_ERR_INVALID_HANDLE 0x01
#define ATT_ERR_ATTR_NOT_FOUND 0x0a
#define ATT_ERR_INSUFFICIENT_AUTHENTICATION 0x05
#define ATT_ERR_INSUFFICIENT_ENCRYPTION 0x0f

/* GATT attribute / service / characteristic UUIDs (16-bit form) */
#define GATT_UUID_PRIMARY_SERVICE 0x2800
#define GATT_UUID_INCLUDE 0x2802
#define GATT_UUID_CHARACTERISTIC 0x2803
#define GATT_UUID_CLIENT_CHAR_CFG 0x2902
#define GATT_UUID_EXT_REPORT_REF 0x2907
#define GATT_UUID_REPORT_REFERENCE 0x2908

#define GATT_SVC_HID 0x1812
#define GATT_CHR_BOOT_KBD_INPUT 0x2a22
#define GATT_CHR_BOOT_KBD_OUTPUT 0x2a32
#define GATT_CHR_BOOT_MOUSE_INPUT 0x2a33
#define GATT_CHR_HID_INFORMATION 0x2a4a
#define GATT_CHR_REPORT_MAP 0x2a4b
#define GATT_CHR_HID_CONTROL_POINT 0x2a4c
#define GATT_CHR_REPORT 0x2a4d
#define GATT_CHR_PROTOCOL_MODE 0x2a4e

/* characteristic declaration value: [props, value handle, uuid] */
#define GATT_CHR_PROP_READ 0x02
#define GATT_CHR_PROP_WRITE_NR 0x04
#define GATT_CHR_PROP_WRITE 0x08
#define GATT_CHR_PROP_NOTIFY 0x10
#define GATT_CHR_PROP_INDICATE 0x20

#define GATT_CCCD_NOTIFY 0x0001
#define GATT_CCCD_INDICATE 0x0002

#define GATT_PROTOCOL_MODE_BOOT 0x00
#define GATT_PROTOCOL_MODE_REPORT 0x01
#define GATT_HID_CTRL_SUSPEND 0x00
#define GATT_HID_CTRL_EXIT_SUSPEND 0x01

/* SMP over the fixed CID 0x0006 (Vol 3 Part H) */
#define SMP_CMD_PAIRING_REQUEST 0x01
#define SMP_CMD_PAIRING_RESPONSE 0x02
#define SMP_CMD_PAIRING_CONFIRM 0x03
#define SMP_CMD_PAIRING_RANDOM 0x04
#define SMP_CMD_PAIRING_FAILED 0x05
#define SMP_CMD_ENCRYPTION_INFO 0x06
#define SMP_CMD_MASTER_IDENT 0x07
#define SMP_CMD_IDENTITY_INFO 0x08
#define SMP_CMD_IDENTITY_ADDR_INFO 0x09
#define SMP_CMD_SIGNING_INFO 0x0a
#define SMP_CMD_SECURITY_REQUEST 0x0b

#define SMP_IO_NO_INPUT_NO_OUTPUT 0x03

#define SMP_AUTHREQ_BONDING 0x01
#define SMP_AUTHREQ_MITM 0x04
#define SMP_AUTHREQ_SC 0x08
#define SMP_AUTHREQ_CT2 0x20

#define SMP_DIST_ENCKEY 0x01
#define SMP_DIST_IDKEY 0x02

#define SMP_REASON_CONFIRM_VALUE_FAILED 0x04
#define SMP_REASON_PAIRING_NOT_SUPPORTED 0x05
#define SMP_REASON_CMD_NOT_SUPPORTED 0x07
#define SMP_REASON_UNSPECIFIED 0x08
#define SMP_REASON_TIMEOUT 0x0c

/* GAP AD structures inside an advertising / scan-response payload */
#define AD_TYPE_FLAGS 0x01
#define AD_TYPE_UUID16_INCOMPLETE 0x02
#define AD_TYPE_UUID16_COMPLETE 0x03
#define AD_TYPE_UUID128_INCOMPLETE 0x06
#define AD_TYPE_UUID128_COMPLETE 0x07
#define AD_TYPE_NAME_SHORT 0x08
#define AD_TYPE_NAME_COMPLETE 0x09
#define AD_TYPE_TX_POWER 0x0a
#define AD_TYPE_APPEARANCE 0x19
#define AD_TYPE_MFG_SPECIFIC 0xff

/* GAP appearance category (top 10 bits) 15 == HID; the sub-categories we
   care about are 961 keyboard, 962 mouse, 963 joystick, 964 gamepad */
#define AD_APPEARANCE_CATEGORY_HID 15

/*
 * LE discovery and classic inquiry never overlap: some Broadcom
 * firmwares answer LE_Set_Scan_Enable with Command Disallowed (0x0c)
 * while an inquiry is in flight and vice versa, so bt_loop runs them as
 * alternating slices. 100% scan duty (window == interval) keeps
 * discovery latency low - a BLE mouse only advertises for ~1s after it
 * wakes up, so missing an interval means missing the device.
 */
#define BT_LE_SCAN_SLICE_MS 2500
#define BT_CLASSIC_SCAN_SLICE_MS 2500
#define BT_LE_SCAN_INTERVAL 0x0030 /* 30 * 0.625ms */
#define BT_LE_SCAN_WINDOW 0x0030
#define BT_LE_SCAN_TYPE_ACTIVE 0x01 /* request SCAN_RSP, which carries the full name */
#define BT_LE_ADDR_TYPE_PUBLIC 0x00
#define BT_LE_ADDR_TYPE_RANDOM 0x01

/* the controller picks a connection interval inside this window; the low
   end is what a mouse wants (7.5ms) and a peripheral that cannot keep it
   up asks for an update over LE signaling, which we accept */
#define BT_LE_CONN_ITV_MIN 0x0006 /* 7.5ms */
#define BT_LE_CONN_ITV_MAX 0x0018 /* 30ms */
#define BT_LE_CONN_LATENCY 0x0000
#define BT_LE_CONN_TIMEOUT 0x01f4 /* 5s */
#define BT_LE_CONN_CE_LEN 0x0000

#define BT_LE_CONNECT_TIMEOUT_MS 12000
#define BT_LE_ATT_TIMEOUT_MS 6000
#define BT_LE_SMP_TIMEOUT_MS 15000

/* our ATT client RX capability, offered in Exchange MTU. The reassembly
   buffer holds a 276-octet L2CAP PDU and l2cap_send_pdu 256 octets of
   payload, so 247 (the practical maximum) fits both ways. */
#define BT_LE_ATT_MTU_PREFERRED 247

/* a bonded LE peripheral only reconnects while it advertises, so the
   power-on autoconnect runs a bounded scan session and lets the first
   advertisement from a known device trigger the bring-up */
#define BT_LE_AUTOCONNECT_SCAN_S 10

/* HOGP fallback path: the Report Map is read in ATT_MTU-sized chunks */
#define BT_MAX_REPORT_MAP 512
#define BT_HOGP_MAX_REPORTS 4
#define BT_MAX_HID_ATTRS 24

/* subscriber fan-out comes from libhid (hid/hid_srv.h) and is identical to
   usbhostd's /dev/hid0: fcntl cmd 0 selects the report id, then read()
   pops fixed-size events from the per-fd queue. Report id 0 stays btd's
   own command/event text stream. */
#define BT_HID_REASSERT_MS 30

/* HIDP transaction headers (high nibble = message type) */
#define HIDP_TRANS_HANDSHAKE 0x00
#define HIDP_TRANS_HID_CONTROL 0x10
#define HIDP_TRANS_SET_PROTOCOL 0x70
#define HIDP_TRANS_DATA 0xA0

#define HIDP_HANDSHAKE_SUCCESS 0x00
#define HIDP_HID_CONTROL_VC_UNPLUG 0x15
#define HIDP_DATA_INPUT 0xA1 /* DATA | input report */

#define HIDP_PROTOCOL_BOOT 0x00

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
    /* LE side. A dual-mode device is one entry carrying both flags; a
       BLE-only HID peripheral (every modern mouse/keyboard) has
       classic == false and is invisible to a BR/EDR inquiry.
       addr_type is part of the identity: the same six octets mean a
       public address or a random one, and LE_Create_Connection takes
       both. */
    bool le;
    bool classic;
    uint8_t addr_type;
    uint16_t appearance;   /* GAP appearance, 0 when not advertised */
    bool adv_hid;          /* advertised the HID Service or a HID appearance */
    uint64_t last_seen_ms; /* LE entries are evicted oldest-first when full */
    /* LE long-term key. A bonded peripheral re-encrypts with this instead
       of running SMP again. */
    bool has_ltk;
    uint8_t ltk[16];
    uint16_t ediv;
    uint8_t ltk_rand[8];
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
    /* LE bonds survive a reboot the same way classic link keys do */
    bool le;
    uint8_t addr_type;
    bool has_ltk;
    uint8_t ltk[16];
    uint16_t ediv;
    uint8_t ltk_rand[8];
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

/* one /dev/bt0 subscriber fd lives in libhid now (fd_info_t): the queue,
   the report-id selection and the directed wakes are identical to
   usbhostd's /dev/hid0, so they are shared rather than duplicated. */

/* ---------------- Bluetooth LE (HOGP) state ----------------
   One LE link at a time: a keyboard or mouse is connected, brought up
   and left alone until it drops, and only then does the next candidate
   get a turn. The bring-up itself is a bounded blocking sequence in the
   same style as the classic pairing path - every wait is on an
   asynchronous controller event and every wait has a deadline. */
typedef enum {
    LE_ST_IDLE = 0,
    LE_ST_CONNECTING,  /* LE_Create_Connection sent */
    LE_ST_LINK_UP,     /* LE Connection Complete seen, link not encrypted */
    LE_ST_PAIRING,     /* SMP phase 1/2 in flight */
    LE_ST_ENCRYPTING,  /* LE_Start_Encryption sent */
    LE_ST_DISCOVERING, /* GATT discovery / CCCD writes in flight */
    LE_ST_READY,       /* input reports subscribed and flowing */
    LE_ST_FAILED
} le_state_t;

typedef struct {
    le_state_t state;
    uint8_t addr[6];
    uint8_t addr_type;
    uint16_t handle;
    bool handle_valid;
    bool encrypted;
    uint64_t deadline_ms;
} le_link_t;

/* ATT client on the fixed CID 0x0004. Exactly one request is outstanding
   at a time - the response, error response or deadline ends the wait. */
typedef struct {
    bool busy;
    uint8_t req_opcode;
    bool rsp_ready;
    uint8_t rsp_opcode;
    uint8_t rsp[256];
    uint16_t rsp_len;
    bool err;
    uint8_t err_code;
    uint16_t err_handle;
} att_client_t;

/* LE Security Manager on the fixed CID 0x0006, legacy pairing with Just
   Works (TK = 0). NoInputNoOutput on both sides is what makes Just Works
   the negotiated method, and our AuthReq clears the SC bit so a
   Secure-Connections-capable peripheral still falls back to the legacy
   flow we implement. */
typedef struct {
    bool active;
    uint8_t preq[7];
    uint8_t pres[7];
    uint8_t mrand[16];   /* our random, over-the-air (little-endian) order */
    uint8_t srand[16];   /* the peer's random, same order */
    uint8_t mconfirm[16];
    uint8_t sconfirm[16];
    uint8_t ltk[16];     /* the LTK we generate and distribute */
    uint16_t ediv;
    uint8_t ltk_rand[8];
    uint8_t peer_ltk[16];
    uint16_t peer_ediv;
    uint8_t peer_rand[8];
    bool got_pres;
    bool got_sconfirm;
    bool got_srand;
    bool got_peer_ltk;
    bool got_peer_ident;
    bool enc_changed;
    bool failed;
    uint8_t fail_reason;
    /* a peripheral usually asks for pairing itself right after the link
       comes up; honour that instead of racing it with our own request */
    bool security_request_seen;
    uint8_t security_request_auth;
} smp_state_t;

/* one discovered characteristic of the HID Service */
typedef struct {
    uint16_t uuid;
    uint8_t props;
    uint16_t decl_handle;  /* the 0x2803 declaration: bounds the descriptor walk */
    uint16_t value_handle;
    uint16_t cccd_handle; /* 0 when it has no Client Characteristic Configuration */
    bool has_report_ref;
    uint8_t report_id;    /* from the Report Reference descriptor */
    uint8_t report_type;  /* 1 = input, 2 = output, 3 = feature */
} hogp_attr_t;

typedef struct {
    bool active;
    uint16_t svc_start;
    uint16_t svc_end;
    uint16_t mtu;
    int n_attrs;
    hogp_attr_t attrs[BT_MAX_HID_ATTRS];
    uint8_t report_map[BT_MAX_REPORT_MAP];
    uint16_t report_map_len;
    bool report_map_complete;
    bool boot_mode_ok;   /* Protocol Mode accepted GATT_PROTOCOL_MODE_BOOT */
    mouse_parser_t mouse;
    bool mouse_ok;       /* Report Map yielded a usable mouse bit layout */
    int n_subscribed;
} hogp_state_t;

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
static uint64_t _sub_reassert_ms = 0;

/* LE link, ATT/GATT client, SMP and HID-over-GATT state */
static le_link_t _le;
static att_client_t _att;
static smp_state_t _smp;
static hogp_state_t _hogp;
static uint8_t _local_addr[6];
static uint8_t _local_addr_type = BT_LE_ADDR_TYPE_PUBLIC;
static bool _le_supported = false;

/* An LE bring-up is queued here instead of running inside the command
   handler: it blocks for seconds and the caller is an IPC command from
   xbt or bt_moused. bt_le_step picks the request up. */
static bool _le_req_active = false;
static uint8_t _le_req_addr[6];
static bool _le_req_pair = false;
static uint64_t _le_req_ms = 0;
static bool _le_autoconnect = false;

/* discovery slicing: _scanning says "a scan session is running",
   _scan_slice says which radio mode currently owns the controller */
typedef enum {
    BT_SCAN_SLICE_NONE = 0,
    BT_SCAN_SLICE_LE,
    BT_SCAN_SLICE_CLASSIC
} bt_scan_slice_t;

static bt_scan_slice_t _scan_slice = BT_SCAN_SLICE_NONE;
static uint64_t _scan_slice_end_ms = 0;
static uint64_t _scan_total_end_ms = 0;
static bool _le_scan_enabled = false;
static bool _inquiry_running = false;

static void l2cap_step(void);
static void l2cap_chan_close(l2cap_chan_t* ch, bool send_req);
static void bt_hid_link_closed(uint16_t handle, const char* reason);
static void bt_hid_dispatch_mouse(const uint8_t* evt);
static void bt_hid_dispatch_keyboard(const uint8_t* evt);
static void bt_hid_start(uint16_t handle, const uint8_t* addr);
static int bt_wait_for_opcode(uint16_t opcode, uint32_t timeout_ms);
static int bt_hci_command_sync_ret(uint16_t ogf, uint16_t ocf,
        const uint8_t* params, uint8_t param_len, uint32_t timeout_ms,
        uint8_t* ret_params, uint8_t ret_cap, uint8_t* ret_len);

/* Bluetooth LE bring-up lives further down (it needs the synchronous HCI
   helpers); these are the entry points the classic paths call into. */
static void bt_handle_le_meta(const uint8_t* payload, size_t len);
static void bt_handle_encryption_change(const uint8_t* payload, size_t len);
static void bt_le_l2cap_rx(uint16_t handle, uint16_t cid,
        const uint8_t* data, size_t len);
static void bt_le_link_closed(uint16_t handle, uint8_t reason);
static void bt_le_step(void);
static int bt_le_connect(bt_device_t* dev, bool pair,
        char* ret_text, size_t ret_text_sz);
static int bt_le_request(bt_device_t* dev, bool pair,
        char* ret_text, size_t ret_text_sz);
static void bt_le_stack_reset(void);
static int bt_le_controller_init(void);
static void bt_hid_handle_report(const uint8_t* data, size_t len);
static void bt_autoconnect_known(void);

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

/* A boot-protocol report is self-identifying by length: a keyboard always
   sends the 8-byte [modifiers, reserved, key1..key6] layout, a mouse the
   3- or 4-byte [buttons, dx, dy, (wheel)] one. We only ever ask for boot
   protocol (SET_PROTOCOL), so the length is enough to route the report to
   the right subscriber report id. */
static void bt_hid_handle_report(const uint8_t* data, size_t len) {
    uint8_t evt[HID_MAX_EVENT_SIZE];

    if (len >= HID_KEYBOARD_REPORT_SIZE) {
        memset(evt, 0, sizeof(evt));
        memcpy(evt, data, HID_KEYBOARD_REPORT_SIZE);
        bt_hid_dispatch_keyboard(evt);
        return;
    }
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
   the queue, the report-id selection and the directed wakes all come from
   libhid, shared verbatim with usbhostd's /dev/hid0. What stays here is
   btd's own twist: report id 0 is not a HID report but the daemon's
   command/event text stream, so open/close/fcntl go straight to libhid
   while read() and check_poll_events() multiplex. */

/* fan one pointer event out; libhid wakes each subscriber only on its
   queue's empty -> non-empty edge (directed proc_wakeup_by). That edge can
   be spent on a generic IPC wait, so arm the bounded re-assert bt_loop
   runs while hid_backlog() still holds. */
static void bt_hid_dispatch_mouse(const uint8_t* evt) {
    if (hid_dispatch_evt(HID_REPORT_ID_MOUSE, evt, HID_POINTER_EVENT_SIZE)) {
        _sub_reassert_ms = kernel_tic_ms(0) + BT_HID_REASSERT_MS;
    }
}

static void bt_hid_dispatch_keyboard(const uint8_t* evt) {
    if (hid_dispatch_evt(HID_REPORT_ID_KEYBOARD, evt, HID_KEYBOARD_EVENT_SIZE)) {
        _sub_reassert_ms = kernel_tic_ms(0) + BT_HID_REASSERT_MS;
    }
}

static int bt_vdev_open(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        int oflag, void* p) {
    return hid_vdev_open(dev, fd, from_pid, node, oflag, p);
}

static int bt_vdev_close(vdevice_t* dev, int fd, int from_pid, ewokos_addr_t node,
        fsinfo_t* fsinfo, void* p) {
    return hid_vdev_close(dev, fd, from_pid, node, fsinfo, p);
}

static int bt_vdev_fcntl(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info,
        int cmd, proto_t* in, proto_t* out, void* p) {
    return hid_vdev_fcntl(dev, fd, from_pid, info, cmd, in, out, p);
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

/* ================= Bluetooth LE (HOGP) =================
   The LE half of the daemon: it discovers the BLE-only mice and keyboards
   a BR/EDR inquiry can never see, then connects, pairs, walks the HID
   Service and subscribes to its input reports. Like the classic pairing
   path it is written as a bounded blocking sequence - send one HCI
   command or one ATT PDU, wait on a predicate with a deadline - and
   bt_poll_once keeps the radio serviced in between. */

/* ---------- AES-128, encryption only ----------
   SMP's security function e is plain AES-128 ECB over a single block with
   the most significant octet of key/plaintext/ciphertext at index 0
   (Vol 3 Part H 2.2.1). The controller could do it for us
   (HCI_LE_Encrypt) but that command's parameter byte order is the reverse
   of e's, so keeping a small encrypt-only AES here means the crypto can
   be checked against the spec's own worked examples instead of against a
   guess about which end an HCI buffer starts at. The S-box is generated
   on first use: a 256-byte constant table costs more flash than the few
   lines that build it, and it is built exactly once per boot. */
static uint8_t _aes_sbox[256];
static bool _aes_sbox_ready = false;

static uint8_t aes_xtime(uint8_t a) {
    return (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1b : 0x00));
}

static uint8_t aes_mul(uint8_t a, uint8_t b) {
    uint8_t r = 0;

    while (b != 0) {
        if (b & 1) {
            r ^= a;
        }
        a = aes_xtime(a);
        b = (uint8_t)(b >> 1);
    }
    return r;
}

/* multiplicative inverse in GF(2^8) by exhaustive search: at most 256
   tries, once per S-box entry, once per boot */
static uint8_t aes_inv(uint8_t a) {
    uint8_t i;

    if (a == 0) {
        return 0;
    }
    for (i = 1; i != 0; ++i) {
        if (aes_mul(a, i) == 1) {
            return i;
        }
    }
    return 0;
}

static void aes_sbox_init(void) {
    int i;

    if (_aes_sbox_ready) {
        return;
    }
    for (i = 0; i < 256; ++i) {
        uint8_t s = aes_inv((uint8_t)i);
        uint8_t v = s;
        int j;

        for (j = 0; j < 4; ++j) {
            v = (uint8_t)((v << 1) | (v >> 7)); /* rotate left within the octet */
            s ^= v;
        }
        _aes_sbox[i] = (uint8_t)(s ^ 0x63);
    }
    _aes_sbox_ready = true;
}

static void aes128_expand_key(const uint8_t key[16], uint8_t rk[176]) {
    static const uint8_t rcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10,
                                     0x20, 0x40, 0x80, 0x1b, 0x36};
    int i;

    aes_sbox_init();
    memcpy(rk, key, 16);
    for (i = 4; i < 44; ++i) {
        uint8_t t[4];
        int j;

        memcpy(t, rk + (i - 1) * 4, 4);
        if ((i % 4) == 0) {
            uint8_t first = t[0];
            t[0] = (uint8_t)(_aes_sbox[t[1]] ^ rcon[i / 4 - 1]);
            t[1] = _aes_sbox[t[2]];
            t[2] = _aes_sbox[t[3]];
            t[3] = _aes_sbox[first];
        }
        for (j = 0; j < 4; ++j) {
            rk[i * 4 + j] = (uint8_t)(rk[(i - 4) * 4 + j] ^ t[j]);
        }
    }
}

/* FIPS-197 maps in[r + 4*c] onto state row r column c, so the byte stream
   and the column-major state are the same array and no transposing is
   needed */
static void aes128_encrypt(const uint8_t key[16], const uint8_t in[16],
        uint8_t out[16]) {
    uint8_t rk[176];
    uint8_t st[16];
    int rnd;
    int c;
    int i;

    aes128_expand_key(key, rk);
    memcpy(st, in, 16);
    for (i = 0; i < 16; ++i) {
        st[i] ^= rk[i];
    }

    for (rnd = 1; rnd <= 10; ++rnd) {
        for (i = 0; i < 16; ++i) {
            st[i] = _aes_sbox[st[i]];
        }
        /* ShiftRows: row r rotates left by r, so rows 1..3 each rotate by
           one, twice and three times */
        for (i = 1; i < 4; ++i) {
            uint8_t first = st[i];
            st[i] = st[i + 4];
            st[i + 4] = st[i + 8];
            st[i + 8] = st[i + 12];
            st[i + 12] = first;
        }
        if (rnd < 10) {
            for (c = 0; c < 4; ++c) {
                uint8_t* s = st + 4 * c;
                uint8_t a0 = s[0];
                uint8_t a1 = s[1];
                uint8_t a2 = s[2];
                uint8_t a3 = s[3];
                uint8_t all = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);

                s[0] = (uint8_t)(a0 ^ all ^ aes_xtime((uint8_t)(a0 ^ a1)));
                s[1] = (uint8_t)(a1 ^ all ^ aes_xtime((uint8_t)(a1 ^ a2)));
                s[2] = (uint8_t)(a2 ^ all ^ aes_xtime((uint8_t)(a2 ^ a3)));
                s[3] = (uint8_t)(a3 ^ all ^ aes_xtime((uint8_t)(a3 ^ a0)));
            }
        }
        for (i = 0; i < 16; ++i) {
            st[i] ^= rk[rnd * 16 + i];
        }
    }
    memcpy(out, st, 16);
}

/* SMP puts every 128-bit value on the wire least-significant octet first
   while e() wants most-significant octet first, so the two orders are a
   plain reverse of each other. */
static void smp_rev16(const uint8_t* in, uint8_t* out) {
    int i;

    for (i = 0; i < 16; ++i) {
        out[i] = in[15 - i];
    }
}

static void smp_e(const uint8_t key_be[16], const uint8_t pt_be[16],
        uint8_t ct_be[16]) {
    aes128_encrypt(key_be, pt_be, ct_be);
}

/* c1(k, r, preq, pres, iat, rat, ia, ra) = e(k, e(k, r XOR p1) XOR p2),
   with every value below shown most-significant octet first (Vol 3 Part H
   2.2.3):
     p1 = pres || preq || rat' || iat'
     p2 = padding(32 zero bits) || ia || ra
   The buffers we hold are all over-the-air (LSB-first), so p1 and p2 are
   assembled by reversing the two 7-octet SMP PDUs and the two 6-octet
   addresses. Verified against the spec's own example: k = 0, r =
   5783D52156AD6F0E6388274EC6702EE0, p1 =
   05000800000302070710000001010001, p2 =
   00000000A1A2A3A4A5A6B1B2B3B4B5B6 yields
   1E1E3FEF878988EAD2A74DC5BEF13B86. */
static void smp_c1(const uint8_t tk[16], const uint8_t r_air[16],
        const uint8_t preq[7], const uint8_t pres[7],
        uint8_t iat, const uint8_t ia[6], uint8_t rat, const uint8_t ra[6],
        uint8_t out_air[16]) {
    uint8_t k_be[16];
    uint8_t p1_be[16];
    uint8_t p2_be[16];
    uint8_t tmp[16];
    int i;

    smp_rev16(tk, k_be);
    smp_rev16(r_air, tmp);

    for (i = 0; i < 7; ++i) {
        p1_be[i] = pres[6 - i];
        p1_be[7 + i] = preq[6 - i];
    }
    p1_be[14] = (uint8_t)(rat & 0x01);
    p1_be[15] = (uint8_t)(iat & 0x01);

    memset(p2_be, 0, 4);
    for (i = 0; i < 6; ++i) {
        p2_be[4 + i] = ia[5 - i];
        p2_be[10 + i] = ra[5 - i];
    }

    for (i = 0; i < 16; ++i) {
        tmp[i] ^= p1_be[i];
    }
    smp_e(k_be, tmp, tmp);
    for (i = 0; i < 16; ++i) {
        tmp[i] ^= p2_be[i];
    }
    smp_e(k_be, tmp, tmp);
    smp_rev16(tmp, out_air);
}

/* s1(k, r1, r2) = e(k, r') with r' = r1' || r2' and r1'/r2' the least
   significant 64 bits of r1/r2 (Vol 3 Part H 2.2.4). The STK is
   s1(TK, LP_RAND_R, LP_RAND_I) - the responder's random comes first. */
static void smp_s1(const uint8_t tk[16], const uint8_t r1_air[16],
        const uint8_t r2_air[16], uint8_t stk_air[16]) {
    uint8_t k_be[16];
    uint8_t r1_be[16];
    uint8_t r2_be[16];
    uint8_t rp_be[16];
    uint8_t tmp[16];

    smp_rev16(tk, k_be);
    smp_rev16(r1_air, r1_be);
    smp_rev16(r2_air, r2_be);
    memcpy(rp_be, r1_be + 8, 8);
    memcpy(rp_be + 8, r2_be + 8, 8);
    smp_e(k_be, rp_be, tmp);
    smp_rev16(tmp, stk_air);
}

/* ---------- random ----------
   The controller's own generator (HCI_LE_Rand) is the good source; the
   xorshift below only covers a controller that refuses it, and is seeded
   from the millisecond clock so two boots do not produce the same LTK. */
static uint32_t _rng_state = 0;

static uint32_t bt_rng32(void) {
    _rng_state ^= _rng_state << 13;
    _rng_state ^= _rng_state >> 17;
    _rng_state ^= _rng_state << 5;
    return _rng_state;
}

static void bt_fill_random(uint8_t* out, size_t n) {
    size_t pos = 0;

    while (pos < n) {
        uint8_t hw[16];
        uint8_t hw_len = 0;

        if (_le_supported &&
                bt_hci_command_sync_ret(HCI_OGF_LE, HCI_OCF_LE_RAND, NULL, 0,
                    1000, hw, sizeof(hw), &hw_len) == 0 && hw_len == 16) {
            size_t take = n - pos < 16 ? n - pos : 16;
            memcpy(out + pos, hw, take);
            pos += take;
            continue;
        }
        if (_rng_state == 0) {
            _rng_state = (uint32_t)kernel_tic_ms(0) ^ 0x5bf03635u;
            if (_rng_state == 0) {
                _rng_state = 0x12345677u;
            }
        }
        out[pos] = (uint8_t)bt_rng32();
        ++pos;
    }
}

/* ---------- bounded waits ----------
   Every LE bring-up step ends in "wait for the controller or the peer to
   answer", and all of them go through here so the deadline is real and
   bt_poll_once keeps the classic paths alive in between. */
typedef bool (*bt_pred_fn)(void* ctx);

static bool bt_poll_until(bt_pred_fn pred, void* ctx, uint32_t timeout_ms) {
    uint64_t start_ms = kernel_tic_ms(0);

    while ((uint32_t)(kernel_tic_ms(0) - start_ms) < timeout_ms) {
        if (pred(ctx)) {
            return true;
        }
        bt_poll_once(2);
    }
    return pred(ctx);
}

/* ---------- controller LE bring-up ----------
   Called from bt_configure_controller. A controller without LE answers
   LE_Set_Event_Mask with Unknown HCI Command and the classic-only path
   carries on; the Pi 5's CYW4345C0 is dual-mode, so this normally
   succeeds. */
static int bt_le_controller_init(void) {
    uint8_t le_mask[8];
    uint8_t addr_ret[8];
    uint8_t addr_ret_len = 0;
    uint8_t buf_ret[8];
    uint8_t buf_ret_len = 0;
    uint16_t le_len = 0;
    uint16_t le_num = 0;

    _le_supported = false;
    _le_scan_enabled = false;
    bt_le_stack_reset();

    memset(le_mask, 0xff, sizeof(le_mask)); /* every LE sub-event */
    if (bt_hci_command_sync(HCI_OGF_LE, HCI_OCF_LE_SET_EVENT_MASK, le_mask,
            sizeof(le_mask), 1000) != 0) {
        slog("bluetooth le unsupported (set_event_mask refused)\n");
        return -1;
    }

    /* c1 hashes both link addresses, so we need our own */
    if (bt_hci_command_sync_ret(HCI_OGF_HOST_CTRL, HCI_OCF_READ_BD_ADDR, NULL, 0,
            1000, addr_ret, sizeof(addr_ret), &addr_ret_len) == 0 &&
            addr_ret_len >= 6) {
        memcpy(_local_addr, addr_ret, 6);
    }

    /* LE_Read_Buffer_Size: [len_lo, len_hi, num]. A zero length means the
       controller shares one ACL pool with BR/EDR, in which case the
       classic count already covers LE. When it reports a separate pool
       the two counts are added: Number_Of_Completed_Packets returns
       completions from both, so one combined counter stays balanced. */
    if (bt_hci_command_sync_ret(HCI_OGF_LE, HCI_OCF_LE_READ_BUFFER_SIZE, NULL, 0,
            1000, buf_ret, sizeof(buf_ret), &buf_ret_len) == 0 &&
            buf_ret_len >= 3) {
        le_len = (uint16_t)((uint16_t)buf_ret[0] | ((uint16_t)buf_ret[1] << 8));
        le_num = buf_ret[2];
    }
    if (le_len != 0 && le_num != 0) {
        _acl_credits = (uint16_t)(_acl_credits + le_num);
    }
    slog("bluetooth le_ready buffer_len=%u buffer_num=%u acl_credits=%u\n",
        le_len, le_num, _acl_credits);

    _le_supported = true;
    return 0;
}

/* ---------- LE scanning ---------- */
static int bt_le_scan_enable(bool enable, bool filter_dup) {
    uint8_t params[2];
    int ret;

    if (!_le_supported) {
        return -1;
    }
    params[0] = enable ? 0x01 : 0x00;
    params[1] = filter_dup ? 0x01 : 0x00;
    ret = bt_hci_command_sync(HCI_OGF_LE, HCI_OCF_LE_SET_SCAN_ENABLE, params,
            sizeof(params), 1000);
    if (ret == 0) {
        _le_scan_enabled = enable;
    }
    else if (enable) {
        slog("bluetooth le_scan_enable_failed status=%d\n", ret);
    }
    return ret;
}

static int bt_le_scan_params(void) {
    uint8_t params[7];

    params[0] = BT_LE_SCAN_TYPE_ACTIVE;
    params[1] = (uint8_t)(BT_LE_SCAN_INTERVAL & 0xff);
    params[2] = (uint8_t)(BT_LE_SCAN_INTERVAL >> 8);
    params[3] = (uint8_t)(BT_LE_SCAN_WINDOW & 0xff);
    params[4] = (uint8_t)(BT_LE_SCAN_WINDOW >> 8);
    params[5] = _local_addr_type; /* our own address type */
    params[6] = 0x00;             /* accept advertisements from anyone */
    return bt_hci_command_sync(HCI_OGF_LE, HCI_OCF_LE_SET_SCAN_PARAMS, params,
            sizeof(params), 1000);
}

/* ---------- GAP advertising data ----------
   An advertising report is a run of length-type-value fields. The three
   we care about are the name (what the user picks from), the 16-bit
   service UUID list (0x1812 is the HID Service) and the appearance
   (category 15 is a HID device, 961 keyboard / 962 mouse). */
static void bt_le_parse_ad(const uint8_t* ad, size_t len, uint16_t* appearance,
        bool* adv_hid, char* name, size_t name_sz) {
    size_t i = 0;

    while (i + 1 < len) {
        uint8_t field_len = ad[i];
        uint8_t type;
        size_t data_len;
        size_t d;

        if (field_len == 0 || i + 1 + (size_t)field_len > len) {
            break;
        }
        type = ad[i + 1];
        data_len = (size_t)field_len - 1;

        if ((type == AD_TYPE_NAME_SHORT || type == AD_TYPE_NAME_COMPLETE) &&
                name != NULL && name_sz > 1 && name[0] == 0) {
            size_t copy = data_len >= name_sz ? name_sz - 1 : data_len;

            for (d = 0; d < copy; ++d) {
                unsigned char ch = ad[i + 2 + d];
                name[d] = isprint(ch) ? (char)ch : '.';
            }
            name[copy] = 0;
            bt_trim_name(name);
        }
        else if (type == AD_TYPE_APPEARANCE && data_len >= 2 &&
                appearance != NULL) {
            *appearance = (uint16_t)((uint16_t)ad[i + 2] |
                    ((uint16_t)ad[i + 3] << 8));
            if (adv_hid != NULL &&
                    (*appearance >> 6) == AD_APPEARANCE_CATEGORY_HID) {
                *adv_hid = true;
            }
        }
        else if ((type == AD_TYPE_UUID16_INCOMPLETE ||
                type == AD_TYPE_UUID16_COMPLETE) && adv_hid != NULL) {
            for (d = 0; d + 1 < data_len; d += 2) {
                uint16_t uuid = (uint16_t)((uint16_t)ad[i + 2 + d] |
                        ((uint16_t)ad[i + 2 + d + 1] << 8));
                if (uuid == GATT_SVC_HID) {
                    *adv_hid = true;
                }
            }
        }
        i += (size_t)field_len + 1;
    }
}

/* The table holds 32 entries and a busy RF neighbourhood advertises far
   more, so an LE slot gets recycled: the oldest entry that is not
   connected, is not a classic device, carries no bond and has no name
   goes first. Only when nothing qualifies is the name requirement
   dropped. */
static bt_device_t* bt_le_alloc_device(const uint8_t* addr) {
    bt_device_t* dev = bt_find_device(addr, true);
    int pass;

    if (dev != NULL) {
        return dev;
    }
    for (pass = 0; pass < 2; ++pass) {
        bt_device_t* victim = NULL;
        uint64_t oldest = 0;
        int i;

        for (i = 0; i < MAX_BT_DEVICES; ++i) {
            bt_device_t* d = &_devices[i];

            if (!d->used || d->connected || d->classic || !d->le) {
                continue;
            }
            if (d->has_ltk || d->has_link_key) {
                continue;
            }
            if (pass == 0 && d->name[0] != 0) {
                continue;
            }
            if (victim == NULL || d->last_seen_ms < oldest) {
                victim = d;
                oldest = d->last_seen_ms;
            }
        }
        if (victim == NULL) {
            continue;
        }
        memset(victim, 0, sizeof(*victim));
        victim->used = true;
        victim->le = true;
        memcpy(victim->addr, addr, 6);
        victim->rssi = 127;
        return victim;
    }
    return NULL;
}

/* Queue an LE bring-up for bt_le_step. Returns false when one is already
   running or queued - the caller reports connect_busy rather than
   silently replacing a request that is mid-flight. */
static bool bt_le_queue_request(const bt_device_t* dev, bool pair) {
    if (dev == NULL || _le_req_active || _le.state != LE_ST_IDLE) {
        return false;
    }
    memcpy(_le_req_addr, dev->addr, 6);
    _le_req_pair = pair;
    _le_req_ms = kernel_tic_ms(0);
    _le_req_active = true;
    return true;
}

/* LE Advertising Report: Num_Reports(1), then per report Event_Type(1)
   Address_Type(1) Address(6) Data_Length(1) Data(n) RSSI(1). */
static void bt_le_handle_adv_report(const uint8_t* p, size_t len) {
    uint8_t n;
    size_t off = 1;
    uint8_t i;

    if (len < 1) {
        return;
    }
    n = p[0];
    for (i = 0; i < n; ++i) {
        uint8_t addr[6];
        uint8_t addr_type;
        uint8_t data_len;
        const uint8_t* data;
        int8_t rssi;
        uint16_t appearance = 0;
        bool adv_hid = false;
        char name[64];
        bt_device_t* dev;
        bool fresh;

        if (off + 9 > len) {
            return;
        }
        addr_type = p[off + 1];
        memcpy(addr, p + off + 2, 6);
        data_len = p[off + 8];
        if (off + 10 + (size_t)data_len > len) {
            return;
        }
        data = p + off + 9;
        rssi = (int8_t)p[off + 9 + data_len];
        off += 10 + (size_t)data_len;

        name[0] = 0;
        bt_le_parse_ad(data, data_len, &appearance, &adv_hid, name, sizeof(name));

        /* A nameless report claiming neither the HID Service nor a HID
           appearance is a beacon or a phone. Admitting those would evict
           real peripherals from the table within seconds, so they are
           dropped unless we already know the address. */
        dev = bt_find_device(addr, false);
        fresh = dev == NULL;
        if (fresh) {
            if (!adv_hid && name[0] == 0) {
                continue;
            }
            dev = bt_le_alloc_device(addr);
            if (dev == NULL) {
                continue;
            }
        }
        dev->le = true;
        dev->addr_type = addr_type;
        dev->rssi = rssi;
        dev->last_seen_ms = kernel_tic_ms(0);
        if (appearance != 0) {
            dev->appearance = appearance;
        }
        if (adv_hid) {
            dev->adv_hid = true;
        }
        if (name[0] != 0 && dev->name[0] == 0) {
            strncpy(dev->name, name, sizeof(dev->name) - 1);
            dev->name[sizeof(dev->name) - 1] = 0;
            bt_trim_name(dev->name);
        }
        if (fresh) {
            bt_emit_device_line("device", dev);
        }

        /* a bonded peripheral that just showed up reconnects by itself */
        if (_le_autoconnect && dev->has_ltk && !dev->connected &&
                (dev->adv_hid || dev->appearance != 0)) {
            char addr_str[24];

            if (bt_le_queue_request(dev, false)) {
                bt_addr_to_str(dev->addr, addr_str, sizeof(addr_str));
                bt_emit("le_autoconnect %s\n", addr_str);
                _le_autoconnect = false;
            }
        }
    }
}

static uint16_t att_le16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* ---------- LE meta events ----------
   LE Connection Complete is 18 octets, LE Enhanced Connection Complete 30
   (it adds the two resolvable private addresses actually used on air).
   We never fill the controller's resolving list, so no address
   resolution happens and Peer_Address is the on-air address SMP's c1 has
   to hash - the RPAs are logged and otherwise ignored. */
static void bt_le_handle_conn_complete(const uint8_t* p, size_t len,
        bool enhanced) {
    uint16_t handle;
    uint16_t interval;
    bt_device_t* dev;
    char addr_str[24];

    if (len < (enhanced ? 30u : 18u)) {
        return;
    }
    handle = (uint16_t)(att_le16(p + 1) & 0x0fff);
    interval = att_le16(enhanced ? p + 23 : p + 11);

    if (p[0] != 0) {
        _le.state = LE_ST_FAILED;
        _le.deadline_ms = kernel_tic_ms(0) + 3000;
        slog("bluetooth le_conn_failed status=0x%02x\n", p[0]);
        return;
    }

    _le.handle = handle;
    _le.handle_valid = true;
    _le.addr_type = p[4];
    memcpy(_le.addr, p + 5, 6);
    _le.encrypted = false;
    if (_le.state != LE_ST_FAILED) {
        _le.state = LE_ST_LINK_UP;
    }

    bt_addr_to_str(_le.addr, addr_str, sizeof(addr_str));
    slog("bluetooth le_connected %s handle=0x%04x role=%u addr_type=%u interval=%u\n",
        addr_str, handle, p[3], p[4], interval);

    dev = bt_find_device(_le.addr, true);
    if (dev != NULL) {
        dev->le = true;
        dev->addr_type = _le.addr_type;
        dev->connected = true;
        dev->handle = handle;
        dev->last_seen_ms = kernel_tic_ms(0);
        bt_emit("le_connected %s handle=0x%04X interval=%u\n",
            addr_str, handle, interval);
    }
}

static void bt_le_handle_conn_update(const uint8_t* p, size_t len) {
    if (len < 9) {
        return;
    }
    slog("bluetooth le_conn_update status=0x%02x handle=0x%04x interval=%u\n",
        p[0], att_le16(p + 1) & 0x0fff, att_le16(p + 3));
}

/* LE Long Term Key Request: the peer's Link Layer wants to re-encrypt and
   names the bond by EDIV + Rand. Answer with the stored LTK when it
   matches, otherwise refuse so SMP runs instead. */
static void bt_le_handle_ltk_request(const uint8_t* p, size_t len) {
    uint8_t params[18];
    uint16_t handle;
    uint16_t ediv;
    bt_device_t* dev;

    if (len < 12) {
        return;
    }
    handle = (uint16_t)(att_le16(p) & 0x0fff);
    ediv = att_le16(p + 10);
    params[0] = (uint8_t)(handle & 0xff);
    params[1] = (uint8_t)(handle >> 8);

    dev = bt_find_device(_le.addr, false);
    if (dev != NULL && dev->has_ltk && dev->ediv == ediv &&
            memcmp(dev->ltk_rand, p + 2, 8) == 0) {
        memcpy(params + 2, dev->ltk, 16);
        _le.encrypted = false;
        (void)bt_hci_command_sync(HCI_OGF_LE, HCI_OCF_LE_LTK_REQ_REPLY, params,
                sizeof(params), 1000);
        slog("bluetooth le_ltk_reply handle=0x%04x ediv=0x%04x\n", handle, ediv);
        return;
    }
    (void)bt_hci_command_sync(HCI_OGF_LE, HCI_OCF_LE_LTK_REQ_NEG_REPLY, params,
            2, 1000);
    slog("bluetooth le_ltk_neg_reply handle=0x%04x ediv=0x%04x\n", handle, ediv);
}

static void bt_handle_le_meta(const uint8_t* payload, size_t len) {
    if (len < 1) {
        return;
    }
    switch (payload[0]) {
    case LE_EVT_CONN_COMPLETE:
        bt_le_handle_conn_complete(payload + 1, len - 1, false);
        break;
    case LE_EVT_ENHANCED_CONN_COMPLETE:
        bt_le_handle_conn_complete(payload + 1, len - 1, true);
        break;
    case LE_EVT_ADV_REPORT:
        bt_le_handle_adv_report(payload + 1, len - 1);
        break;
    case LE_EVT_CONN_UPDATE:
        bt_le_handle_conn_update(payload + 1, len - 1);
        break;
    case LE_EVT_LTK_REQUEST:
        bt_le_handle_ltk_request(payload + 1, len - 1);
        break;
    /* extended advertising reports only arrive after
       LE_Set_Extended_Scan_Enable, which we never send; remote feature
       reads are not part of the bring-up either */
    default:
        break;
    }
}

static void bt_handle_encryption_change(const uint8_t* payload, size_t len) {
    uint16_t handle;

    if (len < 4) {
        return;
    }
    handle = (uint16_t)(att_le16(payload + 1) & 0x0fff);
    if (!_le.handle_valid || handle != _le.handle) {
        return;
    }
    if (payload[0] == 0 && payload[3] != 0) {
        _le.encrypted = true;
        slog("bluetooth le_encrypted handle=0x%04x\n", handle);
    }
    else {
        _le.encrypted = false;
        slog("bluetooth le_encrypt_failed handle=0x%04x status=0x%02x\n",
            handle, payload[0]);
    }
}

/* ---------- ATT client (fixed CID 0x0004) ---------- */
static int att_send(uint16_t handle, const uint8_t* pdu, uint16_t len) {
    return l2cap_send_pdu(handle, L2CAP_CID_ATT, pdu, len);
}

static bool att_rsp_pred(void* ctx) {
    (void)ctx;
    return _att.rsp_ready;
}

/* Exactly one request is outstanding at a time: the matching response, an
   Error Response or the deadline ends the wait. The caller reads _att.rsp
   / _att.rsp_len straight after a zero return. */
static int att_request(uint16_t handle, const uint8_t* pdu, uint16_t len,
        uint8_t expect_op, uint32_t timeout_ms) {
    if (len == 0 || pdu == NULL) {
        return -1;
    }
    memset(&_att, 0, sizeof(_att));
    _att.busy = true;
    _att.req_opcode = pdu[0];
    if (att_send(handle, pdu, len) != 0) {
        _att.busy = false;
        slog("bluetooth att_send_failed req=0x%02x\n", pdu[0]);
        return -1;
    }
    if (!bt_poll_until(att_rsp_pred, NULL, timeout_ms)) {
        _att.busy = false;
        slog("bluetooth att_timeout req=0x%02x\n", pdu[0]);
        return -1;
    }
    _att.busy = false;
    if (_att.err) {
        slog("bluetooth att_error req=0x%02x attr=0x%04x code=0x%02x\n",
            pdu[0], _att.err_handle, _att.err_code);
        return -1;
    }
    if (expect_op != 0 && _att.rsp_opcode != expect_op) {
        slog("bluetooth att_unexpected req=0x%02x rsp=0x%02x\n",
            pdu[0], _att.rsp_opcode);
        return -1;
    }
    return 0;
}

static hogp_attr_t* hogp_find(uint16_t uuid) {
    int i;

    for (i = 0; i < _hogp.n_attrs; ++i) {
        if (_hogp.attrs[i].uuid == uuid) {
            return &_hogp.attrs[i];
        }
    }
    return NULL;
}

static hogp_attr_t* hogp_find_by_value(uint16_t value_handle) {
    int i;

    for (i = 0; i < _hogp.n_attrs; ++i) {
        if (_hogp.attrs[i].value_handle == value_handle) {
            return &_hogp.attrs[i];
        }
    }
    return NULL;
}

/* One input report arrived as a notification or indication. Which
   characteristic it came from decides everything: under Boot Protocol the
   layout is fixed by the HID spec, under Report Protocol the Report Map
   bit layout in _hogp.mouse does the decoding. The bytes handed to the
   subscribers are the same ones a USB or classic-Bluetooth HID device
   produces, because they come out of libhid's normalizer. */
static void bt_le_handle_notify(uint16_t value_handle, const uint8_t* value,
        size_t len) {
    const hogp_attr_t* a = hogp_find_by_value(value_handle);
    uint8_t evt[HID_MAX_EVENT_SIZE];

    if (a == NULL || len == 0) {
        return;
    }

    if (a->uuid == GATT_CHR_BOOT_KBD_INPUT) {
        if (len < HID_KEYBOARD_REPORT_SIZE) {
            return;
        }
        memset(evt, 0, sizeof(evt));
        memcpy(evt, value, HID_KEYBOARD_REPORT_SIZE);
        bt_hid_dispatch_keyboard(evt);
        return;
    }
    if (a->uuid == GATT_CHR_BOOT_MOUSE_INPUT) {
        if (len < HID_MOUSE_REPORT_SIZE) {
            return;
        }
        memset(evt, 0, sizeof(evt));
        evt[0] = value[0]; /* buttons */
        evt[1] = value[1]; /* dx */
        evt[2] = value[2]; /* dy */
        if (len >= 4) {
            evt[3] = value[3]; /* wheel */
        }
        bt_hid_dispatch_mouse(evt);
        return;
    }
    if (a->uuid == GATT_CHR_REPORT) {
        if (_hogp.mouse_ok) {
            const uint8_t* rp = value;
            int rl = (int)len;

            /* a descriptor with a Report Reference means the report is
               prefixed by its id, which the parser does not expect */
            if (_hogp.mouse.has_report_id && rl > 1) {
                ++rp;
                --rl;
            }
            if (mouse_normalize_report(&_hogp.mouse, rp, rl, evt) ==
                    HID_POINTER_EVENT_SIZE) {
                bt_hid_dispatch_mouse(evt);
                return;
            }
        }
        /* keyboard reports keep the boot layout in practice, and anything
           else is routed by length exactly like the classic HIDP path */
        bt_hid_handle_report(value, len);
    }
}

static void att_handle_rx(uint16_t handle, const uint8_t* pdu, size_t len) {
    uint16_t n;

    if (len < 1 || !_le.handle_valid || handle != _le.handle) {
        return;
    }

    switch (pdu[0]) {
    case ATT_OP_ERROR_RSP:
        if (len < 5) {
            return;
        }
        _att.err = true;
        _att.err_handle = att_le16(pdu + 2);
        _att.err_code = pdu[4];
        _att.rsp_ready = true;
        return;
    case ATT_OP_HANDLE_NOTIFY:
    case ATT_OP_HANDLE_IND:
        if (len < 3) {
            return;
        }
        if (pdu[0] == ATT_OP_HANDLE_IND) {
            /* an unconfirmed indication stalls the peer's whole queue */
            uint8_t conf = ATT_OP_IND_CONFIRM;
            (void)att_send(handle, &conf, 1);
        }
        bt_le_handle_notify(att_le16(pdu + 1), pdu + 3, len - 3);
        return;
    default:
        break;
    }

    n = (uint16_t)(len > sizeof(_att.rsp) ? sizeof(_att.rsp) : len);
    memcpy(_att.rsp, pdu, n);
    _att.rsp_len = n;
    _att.rsp_opcode = pdu[0];
    _att.err = false;
    if (pdu[0] == ATT_OP_MTU_RSP && len >= 3) {
        /* ATT_MTU is the smaller of the two sides' RX capabilities */
        uint16_t m = att_le16(pdu + 1);
        if (m >= L2CAP_LE_MTU_DEFAULT && m < _hogp.mtu) {
            _hogp.mtu = m;
        }
    }
    _att.rsp_ready = true;
}

/* ---------- GATT discovery ---------- */
static int gatt_exchange_mtu(uint16_t handle) {
    uint8_t pdu[3];

    _hogp.mtu = BT_LE_ATT_MTU_PREFERRED;
    pdu[0] = ATT_OP_MTU_REQ;
    pdu[1] = (uint8_t)(BT_LE_ATT_MTU_PREFERRED & 0xff);
    pdu[2] = (uint8_t)(BT_LE_ATT_MTU_PREFERRED >> 8);
    if (att_request(handle, pdu, sizeof(pdu), ATT_OP_MTU_RSP,
            BT_LE_ATT_TIMEOUT_MS) != 0) {
        _hogp.mtu = L2CAP_LE_MTU_DEFAULT;
        return -1;
    }
    slog("bluetooth le_att_mtu=%u\n", _hogp.mtu);
    return 0;
}

/* A value longer than ATT_MTU-1 arrives as a Read Response followed by
   Read Blob Responses, each starting one octet further in; a response
   shorter than a full chunk ends the value. */
static int gatt_read_value(uint16_t handle, uint16_t attr, uint8_t* out,
        uint16_t cap, uint16_t* got) {
    uint16_t pos = 0;
    uint16_t chunk = _hogp.mtu > 1 ? (uint16_t)(_hogp.mtu - 1) : 22;
    uint8_t pdu[5];
    bool first = true;

    if (got != NULL) {
        *got = 0;
    }
    for (;;) {
        uint16_t raw;
        uint16_t n;

        if (first) {
            pdu[0] = ATT_OP_READ_REQ;
            pdu[1] = (uint8_t)(attr & 0xff);
            pdu[2] = (uint8_t)(attr >> 8);
            if (att_request(handle, pdu, 3, ATT_OP_READ_RSP,
                    BT_LE_ATT_TIMEOUT_MS) != 0) {
                break;
            }
            first = false;
        }
        else {
            pdu[0] = ATT_OP_READ_BLOB_REQ;
            pdu[1] = (uint8_t)(attr & 0xff);
            pdu[2] = (uint8_t)(attr >> 8);
            pdu[3] = (uint8_t)(pos & 0xff);
            pdu[4] = (uint8_t)(pos >> 8);
            /* Attribute Not Long / Attribute Not Found both mean "that was
               the whole value" rather than a failure */
            if (att_request(handle, pdu, 5, ATT_OP_READ_BLOB_RSP,
                    BT_LE_ATT_TIMEOUT_MS) != 0) {
                break;
            }
        }
        raw = _att.rsp_len > 1 ? (uint16_t)(_att.rsp_len - 1) : 0;
        if (raw == 0) {
            break;
        }
        n = raw;
        if ((uint16_t)(pos + n) > cap) {
            n = (uint16_t)(cap - pos);
        }
        if (n > 0) {
            memcpy(out + pos, _att.rsp + 1, n);
            pos = (uint16_t)(pos + n);
        }
        if (raw < chunk || pos >= cap) {
            break;
        }
    }
    if (got != NULL) {
        *got = pos;
    }
    return pos > 0 ? 0 : -1;
}

static int gatt_write_value(uint16_t handle, uint16_t attr, const uint8_t* v,
        uint16_t n) {
    uint8_t pdu[3 + 64];

    if (n > 64) {
        return -1;
    }
    pdu[0] = ATT_OP_WRITE_REQ;
    pdu[1] = (uint8_t)(attr & 0xff);
    pdu[2] = (uint8_t)(attr >> 8);
    if (n > 0) {
        memcpy(pdu + 3, v, n);
    }
    return att_request(handle, pdu, (uint16_t)(3 + n), ATT_OP_WRITE_RSP,
            BT_LE_ATT_TIMEOUT_MS);
}

static int gatt_write_cccd(uint16_t handle, uint16_t cccd, uint16_t value) {
    uint8_t v[2];

    v[0] = (uint8_t)(value & 0xff);
    v[1] = (uint8_t)(value >> 8);
    return gatt_write_value(handle, cccd, v, 2);
}

/* Read By Group Type over the whole attribute range, looking for the
   0x1812 primary service. An Attribute Not Found error is the normal end
   of the walk, so the loop breaks rather than reporting failure. */
static int gatt_find_hid_service(uint16_t handle) {
    uint8_t pdu[7];
    uint16_t start = 0x0001;

    _hogp.svc_start = 0;
    _hogp.svc_end = 0;

    while (start != 0 && start < 0xffff) {
        uint8_t item;
        uint16_t next = 0;
        size_t n;
        size_t i;

        pdu[0] = ATT_OP_READ_BY_GROUP_REQ;
        pdu[1] = (uint8_t)(start & 0xff);
        pdu[2] = (uint8_t)(start >> 8);
        pdu[3] = 0xff;
        pdu[4] = 0xff;
        pdu[5] = (uint8_t)(GATT_UUID_PRIMARY_SERVICE & 0xff);
        pdu[6] = (uint8_t)(GATT_UUID_PRIMARY_SERVICE >> 8);
        if (att_request(handle, pdu, sizeof(pdu), ATT_OP_READ_BY_GROUP_RSP,
                BT_LE_ATT_TIMEOUT_MS) != 0) {
            break;
        }
        if (_att.rsp_len < 3) {
            break;
        }
        item = _att.rsp[1];
        /* 6 = 16-bit UUID, 20 = 128-bit; the HID Service is a 16-bit one */
        if (item != 6) {
            break;
        }
        n = (_att.rsp_len - 2) / item;
        for (i = 0; i < n; ++i) {
            const uint8_t* e = _att.rsp + 2 + i * item;
            uint16_t group_end = att_le16(e + 2);

            next = (uint16_t)(group_end + 1);
            if (att_le16(e + 4) == GATT_SVC_HID) {
                _hogp.svc_start = att_le16(e);
                _hogp.svc_end = group_end;
                return 0;
            }
        }
        if (next == 0 || next <= start) {
            break;
        }
        start = next;
    }
    return _hogp.svc_start != 0 ? 0 : -1;
}

/* Read By Type for the 0x2803 characteristic declarations inside the HID
   Service. Each item is [decl handle, props, value handle, uuid]. */
static int gatt_discover_chars(uint16_t handle) {
    uint8_t pdu[7];
    uint16_t start = _hogp.svc_start;

    _hogp.n_attrs = 0;
    if (start == 0 || _hogp.svc_end == 0) {
        return -1;
    }
    while (start != 0 && start <= _hogp.svc_end) {
        uint8_t item;
        uint16_t next = 0;
        size_t n;
        size_t i;

        pdu[0] = ATT_OP_READ_BY_TYPE_REQ;
        pdu[1] = (uint8_t)(start & 0xff);
        pdu[2] = (uint8_t)(start >> 8);
        pdu[3] = (uint8_t)(_hogp.svc_end & 0xff);
        pdu[4] = (uint8_t)(_hogp.svc_end >> 8);
        pdu[5] = (uint8_t)(GATT_UUID_CHARACTERISTIC & 0xff);
        pdu[6] = (uint8_t)(GATT_UUID_CHARACTERISTIC >> 8);
        if (att_request(handle, pdu, sizeof(pdu), ATT_OP_READ_BY_TYPE_RSP,
                BT_LE_ATT_TIMEOUT_MS) != 0) {
            break;
        }
        if (_att.rsp_len < 3) {
            break;
        }
        item = _att.rsp[1];
        if (item != 7) {
            break; /* a 128-bit UUID declaration is nothing we handle */
        }
        n = (_att.rsp_len - 2) / item;
        for (i = 0; i < n; ++i) {
            const uint8_t* e = _att.rsp + 2 + i * item;

            next = (uint16_t)(att_le16(e) + 1);
            if (_hogp.n_attrs >= BT_MAX_HID_ATTRS) {
                continue;
            }
            {
                hogp_attr_t* a = &_hogp.attrs[_hogp.n_attrs++];

                memset(a, 0, sizeof(*a));
                a->decl_handle = att_le16(e);
                a->props = e[2];
                a->value_handle = att_le16(e + 3);
                a->uuid = att_le16(e + 5);
            }
        }
        if (next == 0 || next <= start) {
            break;
        }
        start = next;
    }
    return _hogp.n_attrs > 0 ? 0 : -1;
}

/* Find Information over the descriptors of one characteristic: the range
   runs from just past its value to just before the next declaration. The
   CCCD is what makes notifications arrive, the Report Reference is what
   ties a Report characteristic to a report id and direction. */
static void gatt_discover_char_descriptors(uint16_t handle, hogp_attr_t* a) {
    uint16_t start = (uint16_t)(a->value_handle + 1);
    uint16_t end = _hogp.svc_end;
    int j;

    if (a->value_handle == 0 || start == 0) {
        return;
    }
    for (j = 0; j < _hogp.n_attrs; ++j) {
        uint16_t d = _hogp.attrs[j].decl_handle;

        if (d > a->decl_handle && (uint16_t)(d - 1) < end) {
            end = (uint16_t)(d - 1);
        }
    }
    if (start > end) {
        return;
    }

    while (start != 0 && start <= end) {
        uint8_t pdu[5];
        uint8_t fmt;
        size_t item;
        size_t n;
        size_t k;
        uint16_t next = 0;

        pdu[0] = ATT_OP_FIND_INFO_REQ;
        pdu[1] = (uint8_t)(start & 0xff);
        pdu[2] = (uint8_t)(start >> 8);
        pdu[3] = (uint8_t)(end & 0xff);
        pdu[4] = (uint8_t)(end >> 8);
        if (att_request(handle, pdu, sizeof(pdu), ATT_OP_FIND_INFO_RSP,
                BT_LE_ATT_TIMEOUT_MS) != 0) {
            return;
        }
        if (_att.rsp_len < 2) {
            return;
        }
        fmt = _att.rsp[1];
        item = fmt == 0x01 ? 4 : 18; /* 0x01 = 16-bit UUIDs, 0x02 = 128-bit */
        n = (_att.rsp_len - 2) / item;
        for (k = 0; k < n; ++k) {
            const uint8_t* e = _att.rsp + 2 + k * item;
            uint16_t dh = att_le16(e);
            uint16_t uuid = fmt == 0x01 ? att_le16(e + 2) : 0;

            next = (uint16_t)(dh + 1);
            if (uuid == GATT_UUID_CLIENT_CHAR_CFG) {
                a->cccd_handle = dh;
            }
            else if (uuid == GATT_UUID_REPORT_REFERENCE) {
                uint8_t ref[4];
                uint16_t got = 0;

                if (gatt_read_value(handle, dh, ref, sizeof(ref), &got) == 0 &&
                        got >= 2) {
                    a->has_report_ref = true;
                    a->report_id = ref[0];
                    a->report_type = ref[1];
                }
            }
        }
        if (next == 0 || next <= start) {
            return;
        }
        start = next;
    }
}

static int gatt_discover_descriptors(uint16_t handle) {
    int i;

    for (i = 0; i < _hogp.n_attrs; ++i) {
        gatt_discover_char_descriptors(handle, &_hogp.attrs[i]);
    }
    return 0;
}

/* ---- LE bring-up wait predicates ---------------------------------------
   A bring-up blocks on controller events, so every predicate also
   releases on LE_ST_FAILED: the failure path must never leave a caller
   waiting out a full timeout. */

static bool le_link_up_pred(void* ctx) {
    (void)ctx;
    return _le.handle_valid || _le.state == LE_ST_FAILED;
}

static bool le_encrypted_pred(void* ctx) {
    (void)ctx;
    return _le.encrypted || _le.state == LE_ST_FAILED;
}

/* ---- discovery slicing --------------------------------------------------
   bt_start_scan only arms a session; these primitives move the radio
   between LE scanning and classic inquiry and bt_le_step calls them when
   a slice expires. None of them touch _scanning or the session deadline,
   so a bring-up can suspend discovery and let the same session resume
   afterwards without the two mechanisms fighting over the controller. */

static int bt_le_slice_start(void) {
    if (!_le_supported) {
        return -1;
    }
    if (bt_le_scan_params() != 0) {
        return -1;
    }
    return bt_le_scan_enable(true, true);
}

/* Inquiry_Length is counted in 1.28s units, so a shorter slice rounds up;
   the ceiling keeps a long scan session from turning into an inquiry the
   controller cannot be told to stop in time. */
static int bt_classic_inquiry_start(int slice_ms) {
    uint8_t params[5];
    int units;
    int ret;

    if (slice_ms < 1280) {
        slice_ms = 1280;
    }
    units = (slice_ms + 1279) / 1280;
    if (units > 0x30) {
        units = 0x30;
    }
    params[0] = 0x33; /* GIAC LAP 33:8b:9e, the "find everything" code */
    params[1] = 0x8b;
    params[2] = 0x9e;
    params[3] = (uint8_t)units;
    params[4] = 0x00; /* unlimited responses */

    ret = bt_hci_command_sync(HCI_OGF_LINK_CTRL, HCI_OCF_INQUIRY, params,
            sizeof(params), 1500);
    if (ret != 0) {
        slog("bluetooth inquiry_failed status=%d\n", ret);
        return -1;
    }
    _inquiry_running = true;
    return 0;
}

static void bt_scan_slice_stop(void) {
    if (_scan_slice == BT_SCAN_SLICE_LE && _le_scan_enabled) {
        (void)bt_le_scan_enable(false, false);
    }
    if (_scan_slice == BT_SCAN_SLICE_CLASSIC && _inquiry_running) {
        (void)bt_hci_command_sync(HCI_OGF_LINK_CTRL, HCI_OCF_INQUIRY_CANCEL,
                NULL, 0, 1000);
    }
    _inquiry_running = false;
    _le_scan_enabled = false;
}

/* LE_Create_Connection and scanning are mutually exclusive on these
   controllers, so a bring-up takes the radio for itself. _scanning stays
   set: the session resumes once the link is up or the attempt failed. */
static void bt_scan_suspend(void) {
    bt_scan_slice_stop();
    _scan_slice = BT_SCAN_SLICE_NONE;
    _scan_slice_end_ms = 0;
}

static bool bt_scan_enter_le(uint64_t now) {
    if (bt_le_slice_start() != 0) {
        return false;
    }
    _scan_slice = BT_SCAN_SLICE_LE;
    _scan_slice_end_ms = now + BT_LE_SCAN_SLICE_MS;
    return true;
}

static bool bt_scan_enter_classic(uint64_t now) {
    if (bt_classic_inquiry_start(BT_CLASSIC_SCAN_SLICE_MS) != 0) {
        return false;
    }
    _scan_slice = BT_SCAN_SLICE_CLASSIC;
    _scan_slice_end_ms = now + BT_CLASSIC_SCAN_SLICE_MS;
    return true;
}

/* ---- LE Security Manager ------------------------------------------------ */

static int smp_send(uint16_t handle, const uint8_t* pdu, uint16_t len) {
    return l2cap_send_pdu(handle, L2CAP_CID_SMP, pdu, len);
}

static void smp_reset(void) {
    memset(&_smp, 0, sizeof(_smp));
}

static bool smp_pres_pred(void* ctx) {
    (void)ctx;
    return _smp.got_pres || _smp.failed;
}

static bool smp_confirm_pred(void* ctx) {
    (void)ctx;
    return _smp.got_sconfirm || _smp.failed;
}

static bool smp_random_pred(void* ctx) {
    (void)ctx;
    return _smp.got_srand || _smp.failed;
}

static bool smp_keys_pred(void* ctx) {
    (void)ctx;
    return (_smp.got_peer_ltk && _smp.got_peer_ident) || _smp.failed;
}

static bool smp_security_request_pred(void* ctx) {
    (void)ctx;
    return _smp.security_request_seen;
}

/* Pairing Request from the central. NoInputNoOutput with the MITM bit
   clear is what makes Just Works the negotiated method, and clearing the
   SC bit keeps a Secure-Connections-capable peripheral on the legacy flow
   implemented here. RespKeyDist asks for the LTK only: the peripheral's
   LTK is what makes reconnection work, while taking on our own phase-3
   distribution would risk a 30s SMP timeout on the peer. */
static int smp_start_pairing(uint16_t handle) {
    uint8_t pdu[7];

    pdu[0] = SMP_CMD_PAIRING_REQUEST;
    pdu[1] = SMP_IO_NO_INPUT_NO_OUTPUT;
    pdu[2] = 0x00; /* OOB data not available */
    pdu[3] = SMP_AUTHREQ_BONDING;
    pdu[4] = 0x10; /* maximum encryption key size */
    pdu[5] = 0x00; /* initiator key distribution: none */
    pdu[6] = SMP_DIST_ENCKEY;

    memcpy(_smp.preq, pdu, sizeof(pdu));
    _smp.active = true;
    return smp_send(handle, pdu, sizeof(pdu));
}

static void smp_handle_rx(uint16_t handle, const uint8_t* pdu, size_t len) {
    if (len < 1 || !_le.handle_valid || handle != _le.handle) {
        return;
    }

    switch (pdu[0]) {
    case SMP_CMD_PAIRING_RESPONSE:
        if (len >= 7 && !_smp.got_pres) {
            memcpy(_smp.pres, pdu, 7);
            _smp.got_pres = true;
        }
        break;
    case SMP_CMD_PAIRING_CONFIRM:
        if (len >= 17 && !_smp.got_sconfirm) {
            memcpy(_smp.sconfirm, pdu + 1, 16);
            _smp.got_sconfirm = true;
        }
        break;
    case SMP_CMD_PAIRING_RANDOM:
        if (len >= 17 && !_smp.got_srand) {
            memcpy(_smp.srand, pdu + 1, 16);
            _smp.got_srand = true;
        }
        break;
    case SMP_CMD_PAIRING_FAILED:
        _smp.failed = true;
        _smp.fail_reason = len >= 2 ? pdu[1] : 0;
        /* release every outstanding wait at once, whichever one is in it */
        _smp.got_pres = true;
        _smp.got_sconfirm = true;
        _smp.got_srand = true;
        slog("bluetooth smp_failed reason=0x%02x\n", _smp.fail_reason);
        break;
    case SMP_CMD_ENCRYPTION_INFO:
        if (len >= 17) {
            memcpy(_smp.peer_ltk, pdu + 1, 16);
            _smp.got_peer_ltk = true;
        }
        break;
    case SMP_CMD_MASTER_IDENT:
        if (len >= 11) {
            _smp.peer_ediv = att_le16(pdu + 1);
            memcpy(_smp.peer_rand, pdu + 3, 8);
            _smp.got_peer_ident = true;
        }
        break;
    case SMP_CMD_SECURITY_REQUEST:
        _smp.security_request_seen = true;
        _smp.security_request_auth = len >= 2 ? pdu[1] : 0;
        break;
    default:
        /* Identity Info, Identity Address Info and Signing Info are legal
           phase-3 PDUs we never asked for; ignoring beats failing. */
        break;
    }
}

/* LE_Start_Encryption. Random_Number and EDIV are zero whenever the key
   in hand is an STK rather than a distributed LTK (Vol 3 Part H 2.4.4.1). */
static int smp_start_encryption(uint16_t handle, const uint8_t* ltk,
        uint16_t ediv, const uint8_t* rand8) {
    uint8_t params[28];
    int i;

    params[0] = (uint8_t)(handle & 0xff);
    params[1] = (uint8_t)(handle >> 8);
    for (i = 0; i < 8; ++i) {
        params[2 + i] = rand8 != NULL ? rand8[i] : 0;
    }
    params[10] = (uint8_t)(ediv & 0xff);
    params[11] = (uint8_t)(ediv >> 8);
    memcpy(params + 12, ltk, 16);

    _le.encrypted = false;
    _le.state = LE_ST_ENCRYPTING;
    return bt_hci_command_sync(HCI_OGF_LE, HCI_OCF_LE_START_ENCRYPTION,
            params, sizeof(params), 2000);
}

/* Legacy pairing with Just Works: TK is all zeroes, each confirm value is
   c1 over one side's random, and the key that encrypts the link is
   s1(randoms). The whole exchange runs as a bounded blocking sequence,
   the same shape as the classic SSP path it sits next to. */
static int smp_run(uint16_t handle, bt_device_t* dev) {
    uint8_t tk[16];
    uint8_t pdu[17];
    uint8_t confirm[16];
    uint8_t check[16];
    uint8_t stk[16];
    char addr_str[24];

    bt_addr_to_str(_le.addr, addr_str, sizeof(addr_str));
    smp_reset();
    _smp.active = true;
    memset(tk, 0, sizeof(tk));
    bt_fill_random(_smp.mrand, sizeof(_smp.mrand));
    _le.state = LE_ST_PAIRING;

    if (smp_start_pairing(handle) != 0) {
        slog("bluetooth smp_send_failed %s\n", addr_str);
        return -1;
    }
    if (!bt_poll_until(smp_pres_pred, NULL, BT_LE_SMP_TIMEOUT_MS)) {
        slog("bluetooth smp_no_pairing_response %s\n", addr_str);
        return -1;
    }
    if (_smp.failed) {
        return -1;
    }

    smp_c1(tk, _smp.mrand, _smp.preq, _smp.pres, _local_addr_type,
            _local_addr, _le.addr_type, _le.addr, confirm);
    memcpy(_smp.mconfirm, confirm, 16);
    pdu[0] = SMP_CMD_PAIRING_CONFIRM;
    memcpy(pdu + 1, confirm, 16);
    if (smp_send(handle, pdu, sizeof(pdu)) != 0) {
        return -1;
    }
    if (!bt_poll_until(smp_confirm_pred, NULL, BT_LE_SMP_TIMEOUT_MS)) {
        slog("bluetooth smp_no_confirm %s\n", addr_str);
        return -1;
    }
    if (_smp.failed) {
        return -1;
    }

    pdu[0] = SMP_CMD_PAIRING_RANDOM;
    memcpy(pdu + 1, _smp.mrand, 16);
    if (smp_send(handle, pdu, sizeof(pdu)) != 0) {
        return -1;
    }
    if (!bt_poll_until(smp_random_pred, NULL, BT_LE_SMP_TIMEOUT_MS)) {
        slog("bluetooth smp_no_random %s\n", addr_str);
        return -1;
    }
    if (_smp.failed) {
        return -1;
    }

    smp_c1(tk, _smp.srand, _smp.preq, _smp.pres, _local_addr_type,
            _local_addr, _le.addr_type, _le.addr, check);
    if (memcmp(check, _smp.sconfirm, 16) != 0) {
        uint8_t fail[2];

        slog("bluetooth smp_confirm_mismatch %s\n", addr_str);
        fail[0] = SMP_CMD_PAIRING_FAILED;
        fail[1] = SMP_REASON_CONFIRM_VALUE_FAILED;
        (void)smp_send(handle, fail, sizeof(fail));
        return -1;
    }

    /* s1 takes the responder's random first (Vol 3 Part H 2.3.5.5) */
    smp_s1(tk, _smp.srand, _smp.mrand, stk);
    if (smp_start_encryption(handle, stk, 0, NULL) != 0) {
        slog("bluetooth smp_start_encryption_failed %s\n", addr_str);
        return -1;
    }
    if (!bt_poll_until(le_encrypted_pred, NULL, 5000) || !_le.encrypted) {
        slog("bluetooth smp_no_encryption %s\n", addr_str);
        return -1;
    }
    slog("bluetooth smp_paired %s\n", addr_str);

    /* the peripheral distributes its LTK when it set the EncKey bit, and
       that is what turns the next power-on into a re-encryption instead
       of a full pairing round */
    if ((_smp.pres[6] & SMP_DIST_ENCKEY) != 0 && dev != NULL &&
            bt_poll_until(smp_keys_pred, NULL, 3000) &&
            _smp.got_peer_ltk && _smp.got_peer_ident) {
        memcpy(dev->ltk, _smp.peer_ltk, 16);
        dev->ediv = _smp.peer_ediv;
        memcpy(dev->ltk_rand, _smp.peer_rand, 8);
        dev->has_ltk = true;
        slog("bluetooth smp_ltk_stored %s ediv=0x%04x\n", addr_str, dev->ediv);
    }
    return 0;
}

/* ---- HID over GATT bring-up -------------------------------------------- */

static int hogp_subscribe_attr(uint16_t handle, hogp_attr_t* a) {
    uint16_t value;

    if (a->cccd_handle == 0 ||
            (a->props & (GATT_CHR_PROP_NOTIFY | GATT_CHR_PROP_INDICATE)) == 0) {
        return 0;
    }
    value = (a->props & GATT_CHR_PROP_NOTIFY) != 0 ? GATT_CCCD_NOTIFY
                                                   : GATT_CCCD_INDICATE;
    if (gatt_write_cccd(handle, a->cccd_handle, value) != 0) {
        slog("bluetooth le_subscribe_failed uuid=0x%04x cccd=0x%04x\n",
                a->uuid, a->cccd_handle);
        return 0;
    }
    slog("bluetooth le_subscribed uuid=0x%04x value=0x%04x report=%u/%u\n",
            a->uuid, a->value_handle, a->report_id, a->report_type);
    return 1;
}

static int hogp_subscribe(uint16_t handle, uint16_t uuid) {
    hogp_attr_t* a = hogp_find(uuid);

    if (a == NULL) {
        return 0;
    }
    return hogp_subscribe_attr(handle, a);
}

static int hogp_read_report_map(uint16_t handle) {
    hogp_attr_t* a = hogp_find(GATT_CHR_REPORT_MAP);
    uint16_t got = 0;

    if (a == NULL || (a->props & GATT_CHR_PROP_READ) == 0) {
        return -1;
    }
    if (gatt_read_value(handle, a->value_handle, _hogp.report_map,
            sizeof(_hogp.report_map), &got) != 0) {
        return -1;
    }
    _hogp.report_map_len = got;
    _hogp.report_map_complete = true;
    slog("bluetooth le_report_map bytes=%u\n", got);
    return 0;
}

/* Boot Protocol first: a mouse or keyboard that accepts it delivers
   reports in a layout the HID spec fixes, so no descriptor has to be
   parsed at all. Report Protocol is the fallback for peripherals that
   refuse the write or expose no boot characteristics. */
static int hogp_bringup(uint16_t handle) {
    hogp_attr_t* pm;
    uint8_t mode;
    int i;

    memset(&_hogp, 0, sizeof(_hogp));
    _hogp.active = true;
    _hogp.mtu = L2CAP_LE_MTU_DEFAULT;

    if (gatt_exchange_mtu(handle) != 0) {
        /* 23 octets still fits every request we send, so carry on */
        slog("bluetooth le_mtu_exchange_failed mtu=%u\n", _hogp.mtu);
    }
    if (gatt_find_hid_service(handle) != 0) {
        slog("bluetooth le_no_hid_service\n");
        return -1;
    }
    slog("bluetooth le_hid_service range=0x%04x-0x%04x mtu=%u\n",
            _hogp.svc_start, _hogp.svc_end, _hogp.mtu);
    if (gatt_discover_chars(handle) != 0) {
        slog("bluetooth le_no_characteristics\n");
        return -1;
    }
    (void)gatt_discover_descriptors(handle);

    pm = hogp_find(GATT_CHR_PROTOCOL_MODE);
    if (pm != NULL && (pm->props & GATT_CHR_PROP_WRITE) != 0) {
        mode = GATT_PROTOCOL_MODE_BOOT;
        if (gatt_write_value(handle, pm->value_handle, &mode, 1) == 0) {
            _hogp.boot_mode_ok = true;
        }
    }
    if (_hogp.boot_mode_ok) {
        _hogp.n_subscribed = hogp_subscribe(handle, GATT_CHR_BOOT_MOUSE_INPUT) +
                hogp_subscribe(handle, GATT_CHR_BOOT_KBD_INPUT);
        if (_hogp.n_subscribed > 0) {
            slog("bluetooth le_boot_protocol subscribed=%d\n",
                    _hogp.n_subscribed);
            return 0;
        }
    }

    if (pm != NULL && (pm->props & GATT_CHR_PROP_WRITE) != 0) {
        mode = GATT_PROTOCOL_MODE_REPORT;
        (void)gatt_write_value(handle, pm->value_handle, &mode, 1);
    }
    if (hogp_read_report_map(handle) != 0) {
        slog("bluetooth le_no_report_map\n");
        return -1;
    }
    if (hid_parse_mouse_report(_hogp.report_map, (int)_hogp.report_map_len,
            &_hogp.mouse) == 0 &&
            mouse_parser_sane(&_hogp.mouse, 64, false)) {
        _hogp.mouse_ok = true;
    }
    for (i = 0; i < _hogp.n_attrs; ++i) {
        hogp_attr_t* a = &_hogp.attrs[i];

        if (a->uuid != GATT_CHR_REPORT) {
            continue;
        }
        /* report_type 0 means no Report Reference descriptor was there,
           which on an input-only peripheral still means input */
        if (a->report_type != 0 && a->report_type != 1) {
            continue;
        }
        _hogp.n_subscribed += hogp_subscribe_attr(handle, a);
    }
    slog("bluetooth le_report_protocol subscribed=%d mouse_map=%d\n",
            _hogp.n_subscribed, _hogp.mouse_ok ? 1 : 0);
    return _hogp.n_subscribed > 0 ? 0 : -1;
}

/* ---- LE connection, teardown and request queueing ---------------------- */

static void bt_le_fail(const char* reason) {
    char addr_str[24];

    bt_addr_to_str(_le.addr, addr_str, sizeof(addr_str));
    _le.state = LE_ST_FAILED;
    _le.deadline_ms = kernel_tic_ms(0) + 3000;
    bt_emit("connect_fail %s reason=%s\n", addr_str, reason);
    slog("bluetooth le_connect_failed %s reason=%s\n", addr_str, reason);
    if (_le.handle_valid) {
        uint8_t params[3];

        params[0] = (uint8_t)(_le.handle & 0xff);
        params[1] = (uint8_t)(_le.handle >> 8);
        params[2] = 0x13; /* remote user terminated connection */
        (void)bt_hci_command_sync(HCI_OGF_LINK_CTRL, HCI_OCF_DISCONNECT,
                params, sizeof(params), 2000);
    }
}

static void bt_le_stack_reset(void) {
    memset(&_le, 0, sizeof(_le));
    memset(&_att, 0, sizeof(_att));
    memset(&_smp, 0, sizeof(_smp));
    memset(&_hogp, 0, sizeof(_hogp));
    _le.state = LE_ST_IDLE;
}

/* An LE HID link always ends up paired - input reports only flow over an
   encrypted ATT bearer - so the caller's pair flag changes nothing here,
   and a bonded peripheral is re-encrypted instead of going through SMP
   again. */
static int bt_le_connect(bt_device_t* dev, bool pair,
        char* ret_text, size_t ret_text_sz) {
    uint8_t p[25];
    char addr_str[24];
    int i;

    (void)pair;
    bt_le_stack_reset();
    memcpy(_le.addr, dev->addr, 6);
    _le.addr_type = dev->addr_type;
    _le.state = LE_ST_CONNECTING;
    bt_addr_to_str(_le.addr, addr_str, sizeof(addr_str));
    bt_scan_suspend();
    bt_emit("le_connect_begin %s\n", addr_str);

    p[0] = (uint8_t)(BT_LE_SCAN_INTERVAL & 0xff);
    p[1] = (uint8_t)(BT_LE_SCAN_INTERVAL >> 8);
    p[2] = (uint8_t)(BT_LE_SCAN_WINDOW & 0xff);
    p[3] = (uint8_t)(BT_LE_SCAN_WINDOW >> 8);
    p[4] = 0x00; /* filter policy: the peer address, not the white list */
    p[5] = _le.addr_type;
    for (i = 0; i < 6; ++i) {
        p[6 + i] = _le.addr[i];
    }
    p[12] = _local_addr_type;
    p[13] = (uint8_t)(BT_LE_CONN_ITV_MIN & 0xff);
    p[14] = (uint8_t)(BT_LE_CONN_ITV_MIN >> 8);
    p[15] = (uint8_t)(BT_LE_CONN_ITV_MAX & 0xff);
    p[16] = (uint8_t)(BT_LE_CONN_ITV_MAX >> 8);
    p[17] = (uint8_t)(BT_LE_CONN_LATENCY & 0xff);
    p[18] = (uint8_t)(BT_LE_CONN_LATENCY >> 8);
    p[19] = (uint8_t)(BT_LE_CONN_TIMEOUT & 0xff);
    p[20] = (uint8_t)(BT_LE_CONN_TIMEOUT >> 8);
    p[21] = (uint8_t)(BT_LE_CONN_CE_LEN & 0xff);
    p[22] = (uint8_t)(BT_LE_CONN_CE_LEN >> 8);
    p[23] = (uint8_t)(BT_LE_CONN_CE_LEN & 0xff);
    p[24] = (uint8_t)(BT_LE_CONN_CE_LEN >> 8);

    if (bt_hci_command_sync(HCI_OGF_LE, HCI_OCF_LE_CREATE_CONNECTION, p,
            sizeof(p), 2000) != 0) {
        bt_le_stack_reset();
        bt_emit("connect_fail %s reason=le_create_conn\n", addr_str);
        slog("bluetooth le_create_conn_failed %s\n", addr_str);
        return -1;
    }
    if (!bt_poll_until(le_link_up_pred, NULL, BT_LE_CONNECT_TIMEOUT_MS) ||
            !_le.handle_valid) {
        (void)bt_hci_command_sync(HCI_OGF_LE, HCI_OCF_LE_CREATE_CONN_CANCEL,
                NULL, 0, 1000);
        bt_le_stack_reset();
        bt_emit("connect_fail %s reason=le_timeout\n", addr_str);
        slog("bluetooth le_connect_timeout %s\n", addr_str);
        return -1;
    }

    dev = bt_find_device(_le.addr, true);
    if (dev == NULL) {
        bt_le_fail("no_device_slot");
        return -1;
    }

    smp_reset();
    _smp.active = true;
    /* a peripheral usually asks for pairing itself; give it the chance so
       our Pairing Request does not cross with its Security Request */
    (void)bt_poll_until(smp_security_request_pred, NULL, 300);

    if (dev->has_ltk) {
        if (smp_start_encryption(_le.handle, dev->ltk, dev->ediv,
                dev->ltk_rand) == 0 &&
                bt_poll_until(le_encrypted_pred, NULL, 4000) &&
                _le.encrypted) {
            slog("bluetooth le_reencrypted %s\n", addr_str);
        }
        else {
            /* rejected or expired key: drop it and pair from scratch */
            slog("bluetooth le_reencrypt_failed %s\n", addr_str);
            dev->has_ltk = false;
            _le.encrypted = false;
        }
    }
    if (!_le.encrypted && smp_run(_le.handle, dev) != 0) {
        bt_le_fail("pairing");
        return -1;
    }

    _le.state = LE_ST_DISCOVERING;
    if (hogp_bringup(_le.handle) != 0) {
        bt_le_fail("hogp");
        return -1;
    }

    _le.state = LE_ST_READY;
    bt_known_touch_from_device(dev);
    bt_emit("hid_up %s handle=0x%04X le=1 boot=%d reports=%d\n", addr_str,
            _le.handle, _hogp.boot_mode_ok ? 1 : 0, _hogp.n_subscribed);
    slog("bluetooth le_ready %s handle=0x%04x boot=%d reports=%d\n", addr_str,
            _le.handle, _hogp.boot_mode_ok ? 1 : 0, _hogp.n_subscribed);
    if (ret_text != NULL && ret_text_sz > 0) {
        snprintf(ret_text, ret_text_sz, "connect_ok %s handle=0x%04X le=1\n",
                addr_str, _le.handle);
    }
    return 0;
}

static void bt_le_link_closed(uint16_t handle, uint8_t reason) {
    bt_device_t* dev;

    if (!_le.handle_valid || handle != _le.handle) {
        return;
    }
    dev = bt_find_device(_le.addr, false);
    if (dev != NULL) {
        dev->connected = false;
        dev->handle = 0;
    }
    bt_emit("le_disconnected handle=0x%04X reason=%u\n", handle, reason);
    slog("bluetooth le_link_closed handle=0x%04x reason=%u state=%d\n",
            handle, reason, (int)_le.state);
    /* FAILED rather than a full reset: a bring-up in flight is blocked on
       a predicate and has to be released now, and bt_le_step clears the
       whole LE stack once the retry deadline passes. */
    _le.handle_valid = false;
    _le.encrypted = false;
    _le.state = LE_ST_FAILED;
    _le.deadline_ms = kernel_tic_ms(0) + 3000;
}

/* The LE fixed channels never go through L2CAP signalling, so they are
   routed straight out of l2cap_dispatch. LE signalling carries exactly one
   command a peripheral may send us, a connection parameter update request;
   refusing it leaves a mouse that asked for a longer interval retrying
   forever, so it is always accepted. */
static void bt_le_l2cap_rx(uint16_t handle, uint16_t cid,
        const uint8_t* data, size_t len) {
    if (cid == L2CAP_CID_ATT) {
        att_handle_rx(handle, data, len);
        return;
    }
    if (cid == L2CAP_CID_SMP) {
        smp_handle_rx(handle, data, len);
        return;
    }
    if (cid == L2CAP_CID_LE_SIGNAL && len >= 4 &&
            data[0] == L2CAP_SIG_LE_CONN_PARAM_UPDATE_REQ) {
        uint8_t cmd[6];

        cmd[0] = L2CAP_SIG_LE_CONN_PARAM_UPDATE_RSP;
        cmd[1] = data[1]; /* identifier, echoed back */
        cmd[2] = 0x02;    /* data length */
        cmd[3] = 0x00;
        cmd[4] = 0x00;    /* result 0 = accept */
        cmd[5] = 0x00;
        (void)l2cap_send_pdu(handle, L2CAP_CID_LE_SIGNAL, cmd, sizeof(cmd));
    }
}

/* The command-handler entry point. A bring-up blocks for seconds, so it is
   queued and answered later over the event stream: the caller gets
   connect_begin at once and sees hid_up or connect_fail in the events. */
static int bt_le_request(bt_device_t* dev, bool pair,
        char* ret_text, size_t ret_text_sz) {
    char addr_str[24];

    bt_addr_to_str(dev->addr, addr_str, sizeof(addr_str));
    if (_le.state == LE_ST_READY && bt_addr_equal(_le.addr, dev->addr)) {
        if (ret_text != NULL && ret_text_sz > 0) {
            snprintf(ret_text, ret_text_sz,
                    "connect_ok %s handle=0x%04X le=1\n", addr_str, _le.handle);
        }
        return 0;
    }
    if (!_le_supported) {
        if (ret_text != NULL && ret_text_sz > 0) {
            snprintf(ret_text, ret_text_sz,
                    "connect_fail %s reason=le_unsupported\n", addr_str);
        }
        return -1;
    }
    if (!bt_le_queue_request(dev, pair)) {
        if (ret_text != NULL && ret_text_sz > 0) {
            snprintf(ret_text, ret_text_sz, "connect_busy %s\n", addr_str);
        }
        return -1;
    }
    bt_emit("connect_begin %s le=1\n", addr_str);
    if (ret_text != NULL && ret_text_sz > 0) {
        snprintf(ret_text, ret_text_sz, "connect_begin %s le=1\n", addr_str);
    }
    return 0;
}

/* One tick of LE work: failure retry deadlines, queued bring-ups, and the
   discovery slice alternation. Everything that may block for seconds lives
   here rather than in a command handler, which has to stay short enough
   for a click in xbt to feel instant. */
static void bt_le_step(void) {
    uint64_t now = kernel_tic_ms(0);
    bool want_le;

    if (_le.state == LE_ST_FAILED && _le.deadline_ms != 0 &&
            now >= _le.deadline_ms) {
        slog("bluetooth le_reset_after_failure\n");
        bt_le_stack_reset();
    }

    if (_le_req_active) {
        bt_device_t* dev = bt_find_device(_le_req_addr, false);
        bool was_autoconnect = _le_autoconnect;

        _le_req_active = false;
        _le_autoconnect = false;
        if (dev == NULL) {
            char addr_str[24];

            bt_addr_to_str(_le_req_addr, addr_str, sizeof(addr_str));
            bt_emit("connect_fail %s reason=unknown_device\n", addr_str);
            return;
        }
        bt_scan_suspend();
        (void)bt_le_connect(dev, _le_req_pair, NULL, 0);
        if (was_autoconnect) {
            /* the session was armed for autoconnect, so hand the radio to
               the next known device now that this one is up or failed */
            bt_autoconnect_known();
        }
        return;
    }

    /* an LE link owns the radio until it drops */
    if (_le.handle_valid || _le.state != LE_ST_IDLE) {
        return;
    }

    if (!_scanning) {
        _le_autoconnect = false;
        return;
    }
    if (now >= _scan_total_end_ms) {
        bt_scan_slice_stop();
        _scan_slice = BT_SCAN_SLICE_NONE;
        _scan_slice_end_ms = 0;
        _scanning = false;
        _le_autoconnect = false;
        bt_emit("scan_done status=0\n");
        return;
    }
    if (_scan_slice != BT_SCAN_SLICE_NONE && now < _scan_slice_end_ms) {
        return;
    }

    /* the slice expired (or none was armed yet): flip to the other radio
       mode, and only fall back to the one that just ran if the flip
       itself fails. bt_scan_slice_stop leaves _scan_slice alone so the
       choice below still sees which mode is being given up. */
    bt_scan_slice_stop();
    want_le = _scan_slice == BT_SCAN_SLICE_NONE ? _le_supported
                                                : _scan_slice != BT_SCAN_SLICE_LE;
    if (want_le ? bt_scan_enter_le(now) : bt_scan_enter_classic(now)) {
        return;
    }
    if (want_le ? bt_scan_enter_classic(now) : bt_scan_enter_le(now)) {
        return;
    }
    _scan_slice = BT_SCAN_SLICE_NONE;
    _scan_slice_end_ms = now; /* both radios refused: retry next tick */
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
        void* buf, int size, off_t offset, void* p) {
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
