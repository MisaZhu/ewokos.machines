#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/vfs.h>
#include <ewoksys/mmio.h>
#include <ewoksys/dma.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/klog.h>
#include <ewoksys/syscall.h>
#include <arch/bcm283x/mailbox.h>

extern uint32_t _mmio_base;

#define USB_CORE_OFFSET 0x0980000u

#define USB_REPORT_ID_MOUSE 1u
#define USB_REPORT_ID_KEYBOARD 2u
#define USB_REPORT_ID_TOUCH 3u

#define USB_QUEUE_DEPTH 32
#define USB_EVENT_SIZE 7
#define USB_DMA_POOL_SIZE 65536u
#define USB_MAX_INPUTS 8
#define USB_MAX_REPORT 64
#define USB_MAX_CANDIDATES 8
#define USB_MAX_USAGE_LIST 32
#define USB_EVENT_LOG_INTERVAL_MS 500u
#define USB_STATS_LOG_INTERVAL_MS 5000u
#define USB_SCAN_INTERVAL_MS 1000u
#define USB_IDLE_SLEEP_MIN_US 1000u
#define USB_IDLE_SLEEP_MAX_US 50000u
/* interrupt-IN poll pacing: clamp aggressive bInterval values (gaming mice
   advertise 1-4ms) to a sane floor, and stretch the cadence for endpoints
   that keep NAKing -- an idle HID device NAKs every poll and each poll is
   a full channel setup/halt cycle, so backing off cuts idle CPU load */
#define USB_POLL_INTERVAL_MIN_MS 8u
#define USB_POLL_INTERVAL_MAX_MS 40u
#define USB_POLL_IDLE_THRESHOLD 64u
#define USB_LOG_TRANSFER_VERBOSE 0
/* per-transfer errors, poll fail/recover, stats and idle-port traces:
   only wanted when debugging the controller itself */
#define USB_LOG_RUNTIME_VERBOSE 0
/* master switch: bring-up is done, silence all usbhostd logging */
#define USB_LOG_ENABLE 0
#if !USB_LOG_ENABLE
static inline void usb_log_none(const char* fmt, ...) { (void)fmt; }
#define slog(...) usb_log_none(__VA_ARGS__)
#endif
#define DWC_RX_FIFO_SIZE 20480u
#define DWC_NP_TX_FIFO_SIZE 20480u
#define DWC_P_TX_FIFO_SIZE 20480u

#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_REQ_SET_ADDRESS 0x05
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_SET_IDLE 0x0A
#define USB_REQ_SET_PROTOCOL 0x0B
#define USB_REQ_GET_STATUS 0x00
#define USB_REQ_CLEAR_FEATURE 0x01
#define USB_REQ_SET_FEATURE 0x03

#define USB_REQTYPE_STD_IN 0x80
#define USB_REQTYPE_STD_OUT 0x00
#define USB_REQTYPE_STD_IFACE_IN 0x81
#define USB_REQTYPE_CLASS_IFACE_OUT 0x21
#define USB_REQTYPE_CLASS_DEV_IN 0xA0
#define USB_REQTYPE_CLASS_PORT_OUT 0x23
#define USB_REQTYPE_CLASS_PORT_IN 0xA3

#define USB_DESC_DEVICE 0x01
#define USB_DESC_CONFIG 0x02
#define USB_DESC_INTERFACE 0x04
#define USB_DESC_ENDPOINT 0x05
#define USB_DESC_HID 0x21
#define USB_DESC_HID_REPORT 0x22
#define USB_DESC_HUB 0x29

#define USB_CLASS_HUB 0x09

/* hub port features (USB 2.0 spec table 11-17) */
#define USB_HUB_FEAT_PORT_RESET 4
#define USB_HUB_FEAT_PORT_POWER 8
#define USB_HUB_FEAT_C_PORT_CONNECTION 16
#define USB_HUB_FEAT_C_PORT_ENABLE 17
#define USB_HUB_FEAT_C_PORT_RESET 20

/* hub wPortStatus bits */
#define USB_HUB_PS_CONNECTION (1u << 0)
#define USB_HUB_PS_ENABLE (1u << 1)
#define USB_HUB_PS_RESET (1u << 4)
#define USB_HUB_PS_POWER (1u << 8)
#define USB_HUB_PS_LOW_SPEED (1u << 9)

#define USB_CLASS_HID 0x03
#define USB_SUBCLASS_BOOT 0x01
#define USB_PROTOCOL_KEYBOARD 0x01
#define USB_PROTOCOL_MOUSE 0x02

#define USB_ENDPOINT_IN 0x80
#define USB_ENDPOINT_XFER_INTERRUPT 0x03

#define HID_USAGE_PAGE_GENERIC_DESKTOP 0x01
#define HID_USAGE_PAGE_DIGITIZER 0x0D
#define HID_USAGE_POINTER 0x01
#define HID_USAGE_MOUSE 0x02
#define HID_USAGE_JOYSTICK 0x04
#define HID_USAGE_GAMEPAD 0x05
#define HID_USAGE_KEYBOARD 0x06
#define HID_USAGE_TOUCH_SCREEN 0x04
#define HID_USAGE_TOUCH_PAD 0x05
#define HID_USAGE_FINGER 0x22
#define HID_USAGE_TIP_SWITCH 0x42
#define HID_USAGE_X 0x30
#define HID_USAGE_Y 0x31

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
#define DWC_GOTGCTL_ASESVLD (1u << 18)
#define DWC_GOTGCTL_BSESVLD (1u << 19)

#define DWC_GINTSTS_CURMODE_HOST (1u << 0)

#define DWC_GUSBCFG_PHYIF (1u << 3)
#define DWC_GUSBCFG_ULPI_UTMI_SEL (1u << 4)
#define DWC_GUSBCFG_SRPCAP (1u << 8)
#define DWC_GUSBCFG_HNPCAP (1u << 9)
#define DWC_GUSBCFG_ULPI_FSLS (1u << 17)
#define DWC_GUSBCFG_ULPI_DRV_EXT_VBUS (1u << 20)
#define DWC_GUSBCFG_TSDLINEPULSE (1u << 22)
#define DWC_GUSBCFG_ULPI_CLK_SUSP_M (1u << 19)
#define DWC_GUSBCFG_FORCE_HOST_MODE (1u << 29)
#define DWC_GUSBCFG_FORCE_DEV_MODE (1u << 30)

#define DWC_GRSTCTL_CSFTRST (1u << 0)
#define DWC_GRSTCTL_RXFFLSH (1u << 4)
#define DWC_GRSTCTL_TXFFLSH (1u << 5)
#define DWC_GRSTCTL_TXFNUM_SHIFT 6
#define DWC_GRSTCTL_AHB_IDLE (1u << 31)

#define DWC_GINTSTS_PRTINT (1u << 24)
#define DWC_GINTSTS_HCINT (1u << 25)
#define DWC_GINTSTS_DISCONNINT (1u << 29)

#define DWC_HCFG_FSLSPCLKSEL_30_60MHZ 0x0u
#define DWC_HCFG_FSLSSUPP (1u << 2)

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

#define USB_XFER_RETRY (-2)

#define BCM2835_MBOX_TAG_SET_POWER_STATE 0x00028001u
#define BCM2835_MBOX_SET_POWER_STATE_REQ_ON (1u << 0)
#define BCM2835_MBOX_SET_POWER_STATE_REQ_WAIT (1u << 1)
#define BCM2835_MBOX_POWER_DEVID_USB_HCD 3u
#define MAILBOX_VC_ALIAS_NONCACHED 0x40000000u
#define DMA_VC_ALIAS_UNCACHED 0xC0000000u
#define DMA_BUS_ADDR_MASK 0x3FFFFFFFu

typedef struct __attribute__((packed)) {
	uint8_t bmRequestType;
	uint8_t bRequest;
	uint16_t wValue;
	uint16_t wIndex;
	uint16_t wLength;
} usb_setup_pkt_t;

typedef struct __attribute__((packed)) {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint16_t bcdUSB;
	uint8_t bDeviceClass;
	uint8_t bDeviceSubClass;
	uint8_t bDeviceProtocol;
	uint8_t bMaxPacketSize0;
	uint16_t idVendor;
	uint16_t idProduct;
	uint16_t bcdDevice;
	uint8_t iManufacturer;
	uint8_t iProduct;
	uint8_t iSerialNumber;
	uint8_t bNumConfigurations;
} usb_device_desc_t;

typedef struct __attribute__((packed)) {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint16_t wTotalLength;
	uint8_t bNumInterfaces;
	uint8_t bConfigurationValue;
	uint8_t iConfiguration;
	uint8_t bmAttributes;
	uint8_t bMaxPower;
} usb_config_desc_t;

typedef struct __attribute__((packed)) {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bInterfaceNumber;
	uint8_t bAlternateSetting;
	uint8_t bNumEndpoints;
	uint8_t bInterfaceClass;
	uint8_t bInterfaceSubClass;
	uint8_t bInterfaceProtocol;
	uint8_t iInterface;
} usb_iface_desc_t;

typedef struct __attribute__((packed)) {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bEndpointAddress;
	uint8_t bmAttributes;
	uint16_t wMaxPacketSize;
	uint8_t bInterval;
} usb_ep_desc_t;

typedef struct __attribute__((packed)) {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint16_t bcdHID;
	uint8_t bCountryCode;
	uint8_t bNumDescriptors;
	uint8_t bReportDescriptorType;
	uint16_t wReportDescriptorLength;
} usb_hid_desc_t;

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
	uint8_t data[USB_QUEUE_DEPTH][USB_EVENT_SIZE];
	uint8_t len[USB_QUEUE_DEPTH];
	uint8_t rd;
	uint8_t wr;
} usb_queue_t;

typedef struct fd_info {
	int fd;
	int from_pid;
	uint8_t report_id;
	usb_queue_t queue;
	struct fd_info* next;
} fd_info_t;

typedef struct {
	ewokos_addr_t virt;
	uint32_t phys;
	uint32_t size;
	uint32_t used;
} dma_pool_t;

typedef struct {
	bool valid;
	bool has_report_id;
	uint8_t report_id;
	uint8_t report_bytes;
	int tip_bit;
	int tip_size;
	int x_bit;
	int x_size;
	int y_bit;
	int y_size;
	uint32_t x_max;
	uint32_t y_max;
} touch_parser_t;

typedef struct {
	bool valid;
	uint8_t iface_num;
	uint8_t subclass;
	uint8_t protocol;
	uint8_t ep_addr;
	uint8_t interval;
	uint16_t max_packet;
	uint16_t report_desc_len;
} hid_candidate_t;

typedef enum {
	USB_INPUT_NONE = 0,
	USB_INPUT_KEYBOARD,
	USB_INPUT_MOUSE,
	USB_INPUT_TOUCH,
	USB_INPUT_COMPOSITE, /* one interrupt EP carrying kbd+mouse via report IDs */
} usb_input_type_t;

typedef struct {
	bool present;
	usb_input_type_t type;
	uint8_t addr;
	uint8_t iface_num;
	uint8_t ep_addr;
	uint8_t interval;
	uint16_t max_packet;
	uint8_t ctrl_mps;
	bool low_speed;
	uint8_t toggle;
	uint8_t report_len;
	uint8_t kbd_report_id;   /* composite only */
	uint8_t mouse_report_id; /* composite only */
	touch_parser_t touch;
	uint8_t last_report[USB_MAX_REPORT];
	uint8_t last_len;
	uint64_t next_poll_ms;
	uint64_t last_log_ms;
	uint32_t poll_fail_streak;
	uint32_t poll_fail_total;
	uint32_t idle_polls; /* consecutive polls with no data (NAK/fail) */
} usb_input_dev_t;

static dma_pool_t _dma_pool;
static fd_info_t* _fds = NULL;
static usb_input_dev_t _inputs[USB_MAX_INPUTS];
static uint32_t _usb_base = 0;
static bool _device_ready = false;
static bool _port_connected = false;
static uint8_t _next_address = 2;
static uint64_t _next_scan_ms = 0;
static uint32_t _last_hcint = 0;
static uint32_t _num_host_channels = 8;
static uint64_t _next_idle_log_ms = 0;
static uint32_t _enum_fail_streak = 0;

/* cumulative transfer statistics for periodic analysis logging */
typedef struct {
	uint32_t xfer_ok;
	uint32_t xfer_nak;
	uint32_t xfer_err;
	uint32_t xfer_timeout;
	uint32_t xfer_stall;
	uint32_t xfer_txerr;
	uint32_t xfer_bblerr;
	uint32_t xfer_dterr;
	uint32_t xfer_ahberr;
	uint32_t xfer_frmovrun;
	uint32_t halt_timeout;
	uint32_t enum_ok;
	uint32_t enum_fail;
	uint32_t port_disconnect;
} usb_stats_t;

static usb_stats_t _stats;
static uint64_t _next_stats_ms = 0;

static const char* usb_input_type_name(usb_input_type_t type) {
	switch (type) {
	case USB_INPUT_KEYBOARD:
		return "keyboard";
	case USB_INPUT_MOUSE:
		return "mouse";
	case USB_INPUT_TOUCH:
		return "touch";
	case USB_INPUT_COMPOSITE:
		return "composite";
	default:
		return "unknown";
	}
}

static const char* usb_speed_name(bool low_speed) {
	return low_speed ? "low" : "full";
}

/* decode hcint bits into a readable flag string for error analysis */
static const char* dwc_hcint_str(uint32_t hcint) {
	static char buf[96];
	buf[0] = 0;
	if (hcint & DWC_HCINT_XFRC) strcat(buf, "XFRC|");
	if (hcint & DWC_HCINT_CHH) strcat(buf, "CHH|");
	if (hcint & DWC_HCINT_AHBERR) strcat(buf, "AHBERR|");
	if (hcint & DWC_HCINT_STALL) strcat(buf, "STALL|");
	if (hcint & DWC_HCINT_NAK) strcat(buf, "NAK|");
	if (hcint & DWC_HCINT_ACK) strcat(buf, "ACK|");
	if (hcint & DWC_HCINT_NYET) strcat(buf, "NYET|");
	if (hcint & DWC_HCINT_TXERR) strcat(buf, "TXERR|");
	if (hcint & DWC_HCINT_BBLERR) strcat(buf, "BBLERR|");
	if (hcint & DWC_HCINT_FRMOVRUN) strcat(buf, "FRMOVRUN|");
	if (hcint & DWC_HCINT_DTERR) strcat(buf, "DTERR|");
	if (buf[0] == 0) {
		strcpy(buf, "NONE");
	}
	else {
		buf[strlen(buf) - 1] = 0;
	}
	return buf;
}

/* format up to 16 bytes as hex string for raw report analysis */
static const char* usb_hex_str(const uint8_t* data, int len) {
	static char buf[3 * 16 + 4];
	int n = len > 16 ? 16 : len;
	int pos = 0;
	for (int i = 0; i < n; ++i) {
		pos += snprintf(buf + pos, sizeof(buf) - pos, "%02x ", data[i]);
	}
	if (len > 16 && pos < (int)sizeof(buf) - 3) {
		strcpy(buf + pos, "..");
	}
	else if (pos > 0) {
		buf[pos - 1] = 0;
	}
	else {
		buf[0] = 0;
	}
	return buf;
}

static void usb_stats_update_from_hcint(uint32_t hcint) {
	if (hcint & DWC_HCINT_STALL) _stats.xfer_stall++;
	if (hcint & DWC_HCINT_TXERR) _stats.xfer_txerr++;
	if (hcint & DWC_HCINT_BBLERR) _stats.xfer_bblerr++;
	if (hcint & DWC_HCINT_DTERR) _stats.xfer_dterr++;
	if (hcint & DWC_HCINT_AHBERR) _stats.xfer_ahberr++;
	if (hcint & DWC_HCINT_FRMOVRUN) _stats.xfer_frmovrun++;
}

static void usb_log_keyboard_event(const usb_input_dev_t* in, const uint8_t* report, int len) {
	if (len < 8) {
		slog("usbhostd: keybd addr=%u iface=%u short_report len=%d raw=[%s]\n",
				in->addr, in->iface_num, len, usb_hex_str(report, len));
		return;
	}
	slog("usbhostd: keybd addr=%u iface=%u mod=%02x keys=%02x %02x %02x %02x %02x %02x raw=[%s]\n",
			in->addr, in->iface_num, report[0],
			report[2], report[3], report[4], report[5], report[6], report[7],
			usb_hex_str(report, len));
}

static void usb_log_mouse_event(const usb_input_dev_t* in, const uint8_t* report, int len) {
	int8_t dx = 0;
	int8_t dy = 0;
	int8_t wheel = 0;

	if (len > 1) {
		dx = (int8_t)report[1];
	}
	if (len > 2) {
		dy = (int8_t)report[2];
	}
	if (len > 3) {
		wheel = (int8_t)report[3];
	}
	slog("usbhostd: mouse addr=%u iface=%u btn=%02x dx=%d dy=%d wheel=%d len=%d raw=[%s]\n",
			in->addr, in->iface_num, report[0], dx, dy, wheel, len,
			usb_hex_str(report, len));
}

static void usb_log_touch_event(const usb_input_dev_t* in, const uint8_t* payload, const uint8_t* report, int len) {
	uint16_t x = (uint16_t)payload[1] | ((uint16_t)payload[2] << 8);
	uint16_t y = (uint16_t)payload[3] | ((uint16_t)payload[4] << 8);
	slog("usbhostd: touch addr=%u iface=%u pressed=%u x=%u y=%u raw=[%s]\n",
			in->addr, in->iface_num, payload[0], x, y, usb_hex_str(report, len));
}

static void usb_log_input_event(usb_input_dev_t* in, const uint8_t* report, int len, const uint8_t* payload) {
	if (!USB_LOG_ENABLE) {
		return;
	}
	uint64_t now = kernel_tic_ms(0);

	if ((now - in->last_log_ms) < USB_EVENT_LOG_INTERVAL_MS) {
		return;
	}
	in->last_log_ms = now;

	if (in->type == USB_INPUT_KEYBOARD) {
		usb_log_keyboard_event(in, report, len);
	}
	else if (in->type == USB_INPUT_MOUSE) {
		usb_log_mouse_event(in, report, len);
	}
	else if (in->type == USB_INPUT_TOUCH) {
		usb_log_touch_event(in, payload, report, len);
	}
	else if (in->type == USB_INPUT_COMPOSITE && len > 1) {
		/* payload already has the report ID stripped */
		if (report[0] == in->mouse_report_id) {
			usb_log_mouse_event(in, payload, len - 1 > USB_EVENT_SIZE ? USB_EVENT_SIZE : len - 1);
		}
		else {
			usb_log_keyboard_event(in, payload, len - 1 > USB_EVENT_SIZE ? USB_EVENT_SIZE : len - 1);
		}
	}
}

static inline uint16_t le16(const void* p) {
	const uint8_t* b = (const uint8_t*)p;
	return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

static inline uint32_t align_up(uint32_t value, uint32_t align) {
	return (value + align - 1u) & ~(align - 1u);
}

static inline uint32_t usb_dma_bus_addr(uint32_t phys) {
	return (phys & DMA_BUS_ADDR_MASK) | DMA_VC_ALIAS_UNCACHED;
}

static void queue_init(usb_queue_t* queue) {
	memset(queue, 0, sizeof(*queue));
}

static void queue_clear(usb_queue_t* queue) {
	queue->rd = 0;
	queue->wr = 0;
}

static bool queue_has_data(const usb_queue_t* queue) {
	return queue->rd != queue->wr;
}

static void queue_push(usb_queue_t* queue, const uint8_t* data, uint8_t len) {
	if (len > USB_EVENT_SIZE) {
		len = USB_EVENT_SIZE;
	}
	memcpy(queue->data[queue->wr], data, len);
	if (len < USB_EVENT_SIZE) {
		memset(queue->data[queue->wr] + len, 0, USB_EVENT_SIZE - len);
	}
	queue->len[queue->wr] = len;
	queue->wr = (uint8_t)((queue->wr + 1u) % USB_QUEUE_DEPTH);
	if (queue->wr == queue->rd) {
		queue->rd = (uint8_t)((queue->rd + 1u) % USB_QUEUE_DEPTH);
	}
}

static int queue_pop(usb_queue_t* queue, void* buf, int size) {
	int len;
	if (!queue_has_data(queue)) {
		return VFS_ERR_RETRY;
	}
	len = queue->len[queue->rd];
	if (len > size) {
		len = size;
	}
	memcpy(buf, queue->data[queue->rd], len);
	queue->rd = (uint8_t)((queue->rd + 1u) % USB_QUEUE_DEPTH);
	return len;
}

static fd_info_t* fd_find(int fd, int from_pid) {
	fd_info_t* cur = _fds;
	while (cur != NULL) {
		if (cur->fd == fd && cur->from_pid == from_pid) {
			return cur;
		}
		cur = cur->next;
	}
	return NULL;
}

static void fd_add(fd_info_t* item) {
	fd_info_t** tail = &_fds;
	while (*tail != NULL) {
		tail = &((*tail)->next);
	}
	*tail = item;
	item->next = NULL;
}

static void fd_del(int fd, int from_pid) {
	fd_info_t** cur = &_fds;
	while (*cur != NULL) {
		if ((*cur)->fd == fd && (*cur)->from_pid == from_pid) {
			fd_info_t* old = *cur;
			*cur = old->next;
			free(old);
			return;
		}
		cur = &((*cur)->next);
	}
}

static void dispatch_data(uint8_t report_id, const uint8_t* data, uint8_t len) {
	fd_info_t* cur = _fds;
	int delivered = 0;
	while (cur != NULL) {
		if (cur->report_id == report_id) {
			queue_push(&cur->queue, data, len);
			delivered++;
		}
		cur = cur->next;
	}
	if (delivered == 0) {
		if (USB_LOG_RUNTIME_VERBOSE) {
			slog("usbhostd: drop report report_id=%u len=%u no_listener\n", report_id, len);
		}
	}
}

static int dma_pool_init(void) {
	_dma_pool.size = USB_DMA_POOL_SIZE;
	_dma_pool.used = 0;
	_dma_pool.virt = dma_alloc(0, _dma_pool.size);
	if (_dma_pool.virt == 0) {
		slog("usbhostd: dma init alloc_failed size=%u\n", _dma_pool.size);
		return -1;
	}
	_dma_pool.phys = dma_phy_addr(0, _dma_pool.virt);
	memset((void*)(uintptr_t)_dma_pool.virt, 0, _dma_pool.size);
	slog("usbhostd: dma init virt=%08x phys=%08x size=%u\n",
			(uint32_t)_dma_pool.virt, _dma_pool.phys, _dma_pool.size);
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

static void usb_stats_log(void) {
	uint64_t now = kernel_tic_ms(0);
	uint32_t total_err;

	if (!USB_LOG_RUNTIME_VERBOSE) {
		return;
	}
	if (now < _next_stats_ms) {
		return;
	}
	_next_stats_ms = now + USB_STATS_LOG_INTERVAL_MS;

	total_err = _stats.xfer_err + _stats.xfer_timeout;
	/* only log when something noteworthy happened since it is periodic */
	if (total_err == 0 && _stats.halt_timeout == 0 &&
			_stats.enum_fail == 0 && _stats.port_disconnect == 0) {
		return;
	}
	slog("usbhostd: stats ok=%u nak=%u err=%u timeout=%u stall=%u txerr=%u bbl=%u dterr=%u ahberr=%u frmovr=%u halt_to=%u enum_ok=%u enum_fail=%u disc=%u hprt=%08x hfnum=%04x gintsts=%08x\n",
			_stats.xfer_ok, _stats.xfer_nak, _stats.xfer_err, _stats.xfer_timeout,
			_stats.xfer_stall, _stats.xfer_txerr, _stats.xfer_bblerr,
			_stats.xfer_dterr, _stats.xfer_ahberr, _stats.xfer_frmovrun,
			_stats.halt_timeout, _stats.enum_ok, _stats.enum_fail,
			_stats.port_disconnect, usb_readl(DWC_REG_HPRT),
			usb_readl(DWC_REG_HFNUM) & 0xFFFFu, usb_readl(DWC_REG_GINTSTS));
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
	return dwc_wait_grstctl_clear(DWC_GRSTCTL_CSFTRST, 100);
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
		_stats.halt_timeout++;
		if (USB_LOG_RUNTIME_VERBOSE) {
			slog("usbhostd: ch%d halt timeout hcchar=%08x hcint=%08x\n",
					ch, usb_readl(DWC_HCCHAR(ch)), usb_readl(DWC_HCINT(ch)));
		}
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
				_stats.halt_timeout++;
				if (USB_LOG_RUNTIME_VERBOSE) {
					slog("usbhostd: host halt channel_timeout ch=%u hcchar=%08x hcint=%08x\n",
							ch, usb_readl(DWC_HCCHAR(ch)), usb_readl(DWC_HCINT(ch)));
				}
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

static void dwc_port_write(uint32_t set_bits, uint32_t clear_bits) {
	uint32_t reg = usb_readl(DWC_REG_HPRT);
	reg &= ~(DWC_HPRT_ENA | DWC_HPRT_CONNDET | DWC_HPRT_ENCHNG | DWC_HPRT_OVRCURRCHNG);
	reg &= ~clear_bits;
	reg |= set_bits;
	usb_writel(DWC_REG_HPRT, reg);
}

static bool dwc_port_connected(void) {
	return (usb_readl(DWC_REG_HPRT) & 0x1u) != 0;
}

static void dwc_ack_port_change(void) {
	uint32_t reg = usb_readl(DWC_REG_HPRT);
	uint32_t ack = 0;
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

static int dwc_reset_port(bool* low_speed) {
	uint32_t reg;
	uint32_t waited_ms = 0;

	if (USB_LOG_RUNTIME_VERBOSE) {
		slog("usbhostd: port reset begin\n");
	}
	dwc_port_write(DWC_HPRT_PWR, 0);
	proc_usleep(50000);
	if (!dwc_port_connected()) {
		if (USB_LOG_RUNTIME_VERBOSE) {
			slog("usbhostd: port reset abort no_device\n");
		}
		return -1;
	}

	dwc_port_write(DWC_HPRT_PWR | DWC_HPRT_RST, 0);
	proc_usleep(60000);
	dwc_port_write(DWC_HPRT_PWR, DWC_HPRT_RST);
	proc_usleep(10000);
	for (;;) {
		reg = usb_readl(DWC_REG_HPRT);
		if ((reg & 0x1u) == 0) {
			if (USB_LOG_RUNTIME_VERBOSE) {
				slog("usbhostd: port reset failed disconnected\n");
			}
			return -1;
		}
		if ((reg & DWC_HPRT_ENA) != 0) {
			break;
		}
		if (waited_ms++ >= 100u) {
			if (USB_LOG_RUNTIME_VERBOSE) {
				slog("usbhostd: port reset failed not_enabled hprt=%08x\n", reg);
			}
			dwc_ack_port_change();
			return -1;
		}
		proc_usleep(1000);
	}
	dwc_ack_port_change();
	/* USB spec reset recovery: device may ignore traffic for 10ms after reset */
	proc_usleep(20000);

	reg = usb_readl(DWC_REG_HPRT);
	if (!dwc_port_connected()) {
		if (USB_LOG_RUNTIME_VERBOSE) {
			slog("usbhostd: port reset failed disconnected\n");
		}
		return -1;
	}
	if (low_speed != NULL) {
		*low_speed = ((reg & DWC_HPRT_SPEED_MASK) >> DWC_HPRT_SPEED_SHIFT) == 2u;
	}
	slog("usbhostd: port reset done speed=%s hprt=%08x\n",
			usb_speed_name(low_speed != NULL && *low_speed), reg);
	return 0;
}

static int dwc_host_init(void) {
	uint32_t reg;

	slog("usbhostd: host init begin mmio=%08x\n", _mmio_base);
	if (bcm2835_power_on_usb() != 0) {
		slog("usbhostd: host init power_on_failed\n");
		return -1;
	}
	proc_usleep(20000);

	_usb_base = _mmio_base + USB_CORE_OFFSET;

	/* dump core identity/config once: helps confirm the core is powered,
	   clocked and matches expected dwc2 synopsys version */
	slog("usbhostd: core gsnpsid=%08x ghwcfg1=%08x ghwcfg2=%08x ghwcfg3=%08x ghwcfg4=%08x\n",
			usb_readl(DWC_REG_GSNPSID), usb_readl(DWC_REG_GHWCFG1),
			usb_readl(DWC_REG_GHWCFG2), usb_readl(DWC_REG_GHWCFG3),
			usb_readl(DWC_REG_GHWCFG4));
	/* GHWCFG2[17:14] = number of host channels - 1 */
	_num_host_channels = ((usb_readl(DWC_REG_GHWCFG2) >> 14) & 0xFu) + 1u;
	slog("usbhostd: core host_channels=%u\n", _num_host_channels);

	usb_writel(DWC_REG_PCGCR, 0);
	reg = usb_readl(DWC_REG_GUSBCFG);
	/* Internal PHY on BCM283x/BCM2711 is UTMI+ 8-bit: clear ULPI_UTMI_SEL
	   and PHYIF (Linux dwc2 params_bcm2835 does the same). Selecting ULPI
	   here leaves the PHY unclocked on BCM2711 (no connect detect). */
	reg &= ~(DWC_GUSBCFG_PHYIF | DWC_GUSBCFG_ULPI_UTMI_SEL |
			DWC_GUSBCFG_SRPCAP | DWC_GUSBCFG_HNPCAP |
			DWC_GUSBCFG_ULPI_FSLS | DWC_GUSBCFG_ULPI_CLK_SUSP_M |
			DWC_GUSBCFG_ULPI_DRV_EXT_VBUS | DWC_GUSBCFG_TSDLINEPULSE |
			DWC_GUSBCFG_FORCE_DEV_MODE);
	reg |= DWC_GUSBCFG_FORCE_HOST_MODE;
	usb_writel(DWC_REG_GUSBCFG, reg);
	slog("usbhostd: host base=%08x gusbcfg=%08x\n", _usb_base, reg);
	proc_usleep(50000);

	if (dwc_core_soft_reset() != 0) {
		slog("usbhostd: host init soft_reset_failed\n");
		return -1;
	}

	/* Re-assert GUSBCFG after soft reset: ModeSelect (UTMI) survives per spec,
	   but force_host_mode does not. Re-apply to be safe. */
	reg = usb_readl(DWC_REG_GUSBCFG);
	reg &= ~(DWC_GUSBCFG_PHYIF | DWC_GUSBCFG_ULPI_UTMI_SEL |
			DWC_GUSBCFG_SRPCAP | DWC_GUSBCFG_HNPCAP |
			DWC_GUSBCFG_ULPI_FSLS | DWC_GUSBCFG_ULPI_CLK_SUSP_M |
			DWC_GUSBCFG_ULPI_DRV_EXT_VBUS | DWC_GUSBCFG_TSDLINEPULSE |
			DWC_GUSBCFG_FORCE_DEV_MODE);
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

	usb_writel(DWC_REG_HCFG, DWC_HCFG_FSLSPCLKSEL_30_60MHZ | DWC_HCFG_FSLSSUPP);
	reg = usb_readl(DWC_REG_GOTGCTL);
	reg |= DWC_GOTGCTL_HSTSETHNPEN;
	usb_writel(DWC_REG_GOTGCTL, reg);
	if (dwc_host_halt_all_channels() != 0) {
		slog("usbhostd: host init halt_all_channels_failed\n");
		return -1;
	}
	usb_writel(DWC_REG_HAINTMSK, 0);
	usb_writel(DWC_REG_GINTSTS, 0xFFFFFFFFu);
	usb_writel(DWC_REG_GINTMSK, 0);
	reg = usb_readl(DWC_REG_GAHBCFG);
	reg &= ~DWC_GAHBCFG_GLBL_INTR_EN;
	reg |= DWC_GAHBCFG_DMA_EN;
	usb_writel(DWC_REG_GAHBCFG, reg);
	dwc_port_write(DWC_HPRT_PWR, 0);
	proc_usleep(100000);
	dwc_ack_port_change();
	reg = usb_readl(DWC_REG_GINTSTS);
	slog("usbhostd: host init ready gahbcfg=%08x gintmsk=%08x hcfg=%08x hfir=%08x hprt=%08x curmod=%s conid=%s\n",
			usb_readl(DWC_REG_GAHBCFG), usb_readl(DWC_REG_GINTMSK),
			usb_readl(DWC_REG_HCFG), usb_readl(DWC_REG_HFIR), usb_readl(DWC_REG_HPRT),
			(reg & DWC_GINTSTS_CURMODE_HOST) ? "host" : "device",
			(usb_readl(DWC_REG_GOTGCTL) & DWC_GOTGCTL_CONID_B) ? "B" : "A");
	return 0;
}

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
				uint32_t tsiz = usb_readl(DWC_HCTSIZ(ch));
				*actual_out = (uint32_t)(usb_readl(DWC_HCTSIZ(ch)) & DWC_HCTSIZ_XFERSIZE_MASK);
				(void)tsiz;
			}
			return 0;
		}
		proc_usleep(1000);
		waited++;
	}
	return -1;
}

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
		buffer_bus = usb_dma_bus_addr(buffer_phys);
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
			(1u << DWC_HCCHAR_MC_SHIFT) |
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
	if (USB_LOG_TRANSFER_VERBOSE && ep_num == 0) {
		slog("usbhostd: ch%d start addr=%u ep=%u dir=%s type=%u pid=%u mps=%u len=%u dma_phys=%08x dma_bus=%08x hcchar=%08x hctsiz=%08x hcsplt=%08x hprt=%08x\n",
				ch, dev_addr, ep_num, dir_in ? "in" : "out", ep_type, pid,
				max_packet, length, buffer_phys, buffer_bus, hcchar_start, hctsiz,
				usb_readl(DWC_HCSPLT(ch)), usb_readl(DWC_REG_HPRT));
	}
	dwc_writel_sync(DWC_HCCHAR(ch), hcchar_start);

	if (dwc_channel_wait(ch, timeout_ms, &hcint, &remaining) != 0) {
		_stats.xfer_timeout++;
		_last_hcint = usb_readl(DWC_HCINT(ch));
		if (USB_LOG_RUNTIME_VERBOSE) {
			slog("usbhostd: ch%d timeout addr=%u ep=%u dir=%s len=%u pid=%u hcchar=%08x hcint=%08x hctsiz=%08x hprt=%08x hfnum=%04x\n",
					ch, dev_addr, ep_num, dir_in ? "in" : "out", length, pid,
					usb_readl(DWC_HCCHAR(ch)), usb_readl(DWC_HCINT(ch)),
					usb_readl(DWC_HCTSIZ(ch)), usb_readl(DWC_REG_HPRT),
					usb_readl(DWC_REG_HFNUM) & 0xFFFFu);
		}
		(void)dwc_channel_halt(ch, 20);
		return -1;
	}

	usb_writel(DWC_HCINT(ch), hcint);
	_last_hcint = hcint;
	if (hcint & (DWC_HCINT_AHBERR | DWC_HCINT_STALL | DWC_HCINT_TXERR |
			DWC_HCINT_BBLERR | DWC_HCINT_DTERR)) {
		_stats.xfer_err++;
		usb_stats_update_from_hcint(hcint);
		if (USB_LOG_RUNTIME_VERBOSE) {
			slog("usbhostd: ch%d error addr=%u ep=%u dir=%s hcint=%08x(%s) hcchar=%08x hctsiz=%08x len=%u\n",
					ch, dev_addr, ep_num, dir_in ? "in" : "out", hcint, dwc_hcint_str(hcint),
					usb_readl(DWC_HCCHAR(ch)), usb_readl(DWC_HCTSIZ(ch)), length);
		}
		(void)dwc_channel_halt(ch, 20);
		return -1;
	}
	if (hcint & (DWC_HCINT_NAK | DWC_HCINT_NYET | DWC_HCINT_FRMOVRUN)) {
		_stats.xfer_nak++;
		usb_stats_update_from_hcint(hcint);
		(void)dwc_channel_halt(ch, 20);
		return USB_XFER_RETRY;
	}
	if ((hcint & DWC_HCINT_XFRC) == 0 && (hcint & DWC_HCINT_CHH) != 0 && length != 0) {
		_stats.xfer_err++;
		if (USB_LOG_RUNTIME_VERBOSE) {
			slog("usbhostd: ch%d halted addr=%u ep=%u dir=%s hcint=%08x(%s) hcchar=%08x hctsiz=%08x len=%u\n",
					ch, dev_addr, ep_num, dir_in ? "in" : "out", hcint, dwc_hcint_str(hcint),
					usb_readl(DWC_HCCHAR(ch)), usb_readl(DWC_HCTSIZ(ch)), length);
		}
		(void)dwc_channel_halt(ch, 20);
		return -1;
	}
	_stats.xfer_ok++;
	if (USB_LOG_TRANSFER_VERBOSE && ep_num == 0) {
		slog("usbhostd: ch%d done addr=%u ep=%u dir=%s pid=%u hcint=%08x remain=%u actual=%d hprt=%08x\n",
				ch, dev_addr, ep_num, dir_in ? "in" : "out", pid, hcint,
				usb_readl(DWC_HCTSIZ(ch)) & DWC_HCTSIZ_XFERSIZE_MASK,
				(int)(length - (usb_readl(DWC_HCTSIZ(ch)) & DWC_HCTSIZ_XFERSIZE_MASK)),
				usb_readl(DWC_REG_HPRT));
	}
	return (int)(length - (usb_readl(DWC_HCTSIZ(ch)) & DWC_HCTSIZ_XFERSIZE_MASK));
}

static int dwc_control_stage_transfer(const char* stage_name, int ch, uint8_t addr,
		uint8_t ep_num, bool dir_in, bool low_speed, uint8_t ep_type,
		uint16_t max_packet, uint32_t pid, uint32_t buffer_phys,
		uint32_t length, uint32_t timeout_ms, uint8_t req) {
	int attempt;
	int ret = -1;

	for (attempt = 1; attempt <= 3; attempt++) {
		ret = dwc_channel_transfer(ch, addr, ep_num, dir_in, low_speed, ep_type,
				max_packet, pid, buffer_phys, length, timeout_ms);
		if (ret >= 0) {
			if (USB_LOG_TRANSFER_VERBOSE && attempt > 1) {
				slog("usbhostd: ctrl %s retry_ok addr=%u req=%02x attempt=%d actual=%d\n",
						stage_name, addr, req, attempt, ret);
			}
			return ret;
		}
		if (USB_LOG_TRANSFER_VERBOSE && attempt < 3) {
			slog("usbhostd: ctrl %s retry addr=%u req=%02x attempt=%d\n",
					stage_name, addr, req, attempt + 1);
			proc_usleep(5000);
		}
		else if (attempt < 3) {
			proc_usleep(5000);
		}
	}
	return ret;
}

static int usb_control_msg(uint8_t addr, bool low_speed, uint8_t ep_mps,
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
	if (USB_LOG_TRANSFER_VERBOSE) {
		slog("usbhostd: ctrl setup addr=%u speed=%s bmReq=%02x req=%02x wValue=%04x wIndex=%04x wLength=%u mps=%u\n",
				addr, usb_speed_name(low_speed), setup->bmRequestType, setup->bRequest,
				setup->wValue, setup->wIndex, setup->wLength, ep_mps);
	}

	if (setup->wLength > 0) {
		payload = (uint8_t*)dma_pool_alloc(setup->wLength, 8, &payload_phys);
		if (payload == NULL) {
			slog("usbhostd: ctrl payload alloc_failed len=%u\n", setup->wLength);
			dma_pool_rewind(mark);
			return -1;
		}
		if (!data_in && data != NULL) {
			memcpy(payload, data, setup->wLength);
		}
	}

	status_zlp = (uint8_t*)dma_pool_alloc(8, 8, &status_zlp_phys);
	if (status_zlp == NULL) {
		slog("usbhostd: ctrl status_zlp alloc_failed\n");
		dma_pool_rewind(mark);
		return -1;
	}
	(void)status_zlp;

	ret = dwc_control_stage_transfer("setup", 0, addr, 0, false, low_speed, 0,
			ep_mps, DWC_PID_SETUP, setup_phys, sizeof(*setup_dma), 200,
			setup->bRequest);
	if (ret < 0) {
		if (USB_LOG_RUNTIME_VERBOSE) {
			slog("usbhostd: ctrl setup_stage_failed addr=%u req=%02x\n", addr, setup->bRequest);
		}
		dma_pool_rewind(mark);
		return -1;
	}
	if (USB_LOG_TRANSFER_VERBOSE) {
		slog("usbhostd: ctrl setup_stage_ok addr=%u req=%02x actual=%d\n", addr, setup->bRequest, ret);
	}

	if (setup->wLength > 0) {
		ret = dwc_control_stage_transfer("data", 0, addr, 0, data_in, low_speed, 0,
				ep_mps, DWC_PID_DATA1, payload_phys, setup->wLength, 500,
				setup->bRequest);
		if (ret < 0) {
			if (USB_LOG_RUNTIME_VERBOSE) {
				slog("usbhostd: ctrl data_stage_failed addr=%u req=%02x dir=%s len=%u\n",
						addr, setup->bRequest, data_in ? "in" : "out", setup->wLength);
			}
			dma_pool_rewind(mark);
			return -1;
		}
		actual = ret;
		if (USB_LOG_TRANSFER_VERBOSE) {
			slog("usbhostd: ctrl data_stage_ok addr=%u req=%02x dir=%s actual=%d len=%u\n",
					addr, setup->bRequest, data_in ? "in" : "out", ret, setup->wLength);
		}
		if (data_in && data != NULL && ret > 0) {
			memcpy(data, payload, ret);
		}
	}

	ret = dwc_control_stage_transfer("status", 0, addr, 0, !data_in, low_speed, 0,
			ep_mps, DWC_PID_DATA1, status_zlp_phys, 0, 200, setup->bRequest);
	dma_pool_rewind(mark);
	if (ret < 0) {
		if (USB_LOG_RUNTIME_VERBOSE) {
			slog("usbhostd: ctrl status_stage_failed addr=%u req=%02x dir=%s\n",
					addr, setup->bRequest, data_in ? "out" : "in");
		}
		return -1;
	}
	if (USB_LOG_TRANSFER_VERBOSE) {
		slog("usbhostd: ctrl status_stage_ok addr=%u req=%02x\n", addr, setup->bRequest);
	}
	return actual;
}

static int usb_interrupt_in(usb_input_dev_t* dev, void* data, uint16_t size) {
	uint32_t mark = dma_pool_mark();
	uint8_t* payload;
	uint32_t payload_phys = 0;
	int ret;

	payload = (uint8_t*)dma_pool_alloc(size, 8, &payload_phys);
	if (payload == NULL) {
		return -1;
	}

	ret = dwc_channel_transfer(1, dev->addr, (uint8_t)(dev->ep_addr & 0x0Fu), true,
			dev->low_speed, 3, dev->max_packet, dev->toggle ? DWC_PID_DATA1 : DWC_PID_DATA0,
			payload_phys, size, 50);
	if (ret == USB_XFER_RETRY) {
		dma_pool_rewind(mark);
		return 0;
	}
	if (ret >= 0) {
		if (ret > 0) {
			memcpy(data, payload, ret);
		}
		/* resync toggle from the core: HCTSIZ.PID holds the pid for the
		   next transaction and advances on every accepted packet, ZLPs
		   included (a blind xor skips ZLPs and desyncs -> DTERR storms) */
		uint32_t hwpid = (usb_readl(DWC_HCTSIZ(1)) >> DWC_HCTSIZ_PID_SHIFT) & 0x3u;
		dev->toggle = (hwpid == DWC_PID_DATA1) ? 1u : 0u;
	}
	else if (_last_hcint & DWC_HCINT_DTERR) {
		/* device toggle is authoritative: it only advances on our ACK, so a
		   toggle error means our expectation is stale -- flip and retry the
		   next poll instead of surfacing a hard failure */
		dev->toggle ^= 1u;
		dma_pool_rewind(mark);
		return 0;
	}
	dma_pool_rewind(mark);
	return ret;
}

static int usb_get_descriptor(uint8_t addr, bool low_speed, uint8_t ep_mps,
		uint8_t req_type, uint8_t desc_type, uint8_t desc_index, uint16_t index,
		void* buf, uint16_t len) {
	usb_setup_pkt_t setup;
	memset(&setup, 0, sizeof(setup));
	setup.bmRequestType = req_type;
	setup.bRequest = USB_REQ_GET_DESCRIPTOR;
	setup.wValue = (uint16_t)(((uint16_t)desc_type << 8) | desc_index);
	setup.wIndex = index;
	setup.wLength = len;
	return usb_control_msg(addr, low_speed, ep_mps, &setup, buf, true);
}

static int usb_set_address(bool low_speed, uint8_t new_addr) {
	usb_setup_pkt_t setup;
	memset(&setup, 0, sizeof(setup));
	setup.bmRequestType = USB_REQTYPE_STD_OUT;
	setup.bRequest = USB_REQ_SET_ADDRESS;
	setup.wValue = new_addr;
	return usb_control_msg(0, low_speed, 8, &setup, NULL, false);
}

static int usb_set_configuration(uint8_t addr, bool low_speed, uint8_t ep_mps, uint8_t config_value) {
	usb_setup_pkt_t setup;
	memset(&setup, 0, sizeof(setup));
	setup.bmRequestType = USB_REQTYPE_STD_OUT;
	setup.bRequest = USB_REQ_SET_CONFIGURATION;
	setup.wValue = config_value;
	return usb_control_msg(addr, low_speed, ep_mps, &setup, NULL, false);
}

static int usb_hid_set_idle(uint8_t addr, bool low_speed, uint8_t ep_mps, uint8_t iface_num) {
	usb_setup_pkt_t setup;
	memset(&setup, 0, sizeof(setup));
	setup.bmRequestType = USB_REQTYPE_CLASS_IFACE_OUT;
	setup.bRequest = USB_REQ_SET_IDLE;
	setup.wIndex = iface_num;
	setup.wValue = 0;
	return usb_control_msg(addr, low_speed, ep_mps, &setup, NULL, false);
}

static int usb_hid_set_protocol(uint8_t addr, bool low_speed, uint8_t ep_mps, uint8_t iface_num, uint8_t protocol) {
	usb_setup_pkt_t setup;
	memset(&setup, 0, sizeof(setup));
	setup.bmRequestType = USB_REQTYPE_CLASS_IFACE_OUT;
	setup.bRequest = USB_REQ_SET_PROTOCOL;
	setup.wIndex = iface_num;
	setup.wValue = protocol;
	return usb_control_msg(addr, low_speed, ep_mps, &setup, NULL, false);
}

static void clear_local_usages(uint32_t* usages, int* usage_count, bool* usage_range_valid) {
	(void)usages;
	*usage_count = 0;
	*usage_range_valid = false;
}

static uint32_t hid_usage_for_index(const uint32_t* usages, int usage_count,
		bool usage_range_valid, uint32_t usage_min, uint32_t usage_max, int idx) {
	if (usage_count > 0) {
		if (idx < usage_count) {
			return usages[idx];
		}
		return usages[usage_count - 1];
	}
	if (usage_range_valid) {
		uint32_t usage = usage_min + (uint32_t)idx;
		if (usage > usage_max) {
			usage = usage_max;
		}
		return usage;
	}
	return 0xFFFFFFFFu;
}

static int32_t hid_sign_extend(uint32_t value, int bits) {
	if (bits <= 0 || bits >= 32) {
		return (int32_t)value;
	}
	if ((value & (1u << (bits - 1))) != 0) {
		value |= ~((1u << bits) - 1u);
	}
	return (int32_t)value;
}

typedef enum {
	HID_DEV_TYPE_UNKNOWN = 0,
	HID_DEV_TYPE_KEYBOARD,
	HID_DEV_TYPE_MOUSE,
	HID_DEV_TYPE_TOUCH,
} hid_dev_type_t;

static hid_dev_type_t hid_detect_device_type(const uint8_t* desc, int len) {
	uint32_t usage_page = 0;
	uint32_t usages[USB_MAX_USAGE_LIST];
	int usage_count = 0;
	uint32_t usage_min = 0;
	uint32_t usage_max = 0;
	bool usage_range_valid = false;

	for (int off = 0; off < len; ) {
		uint8_t prefix = desc[off++];
		uint32_t value = 0;
		int size_code, size, type, tag;

		if (prefix == 0xFE) {
			if (off + 2 > len) break;
			size = desc[off];
			off += 2 + size;
			continue;
		}

		size_code = prefix & 0x3;
		size = (size_code == 3) ? 4 : size_code;
		type = (prefix >> 2) & 0x3;
		tag = (prefix >> 4) & 0xF;
		if (off + size > len) break;
		for (int i = 0; i < size; ++i) {
			value |= (uint32_t)desc[off + i] << (i * 8);
		}
		off += size;

		if (type == 1) {
			if (tag == 0) {
				usage_page = value;
			}
		}
		else if (type == 2) {
			switch (tag) {
			case 0:
				if (usage_count < USB_MAX_USAGE_LIST) {
					usages[usage_count++] = value;
				}
				break;
			case 1:
				usage_min = value;
				usage_range_valid = true;
				break;
			case 2:
				usage_max = value;
				usage_range_valid = true;
				break;
			}
		}
		else if (type == 0) {
			if (tag == 10) {
				uint8_t collection_type = (uint8_t)value;
				if (collection_type == 1 && usage_page == HID_USAGE_PAGE_GENERIC_DESKTOP) {
					uint32_t usage = hid_usage_for_index(usages, usage_count,
							usage_range_valid, usage_min, usage_max, 0);
					if (usage == HID_USAGE_KEYBOARD) {
						return HID_DEV_TYPE_KEYBOARD;
					}
					else if (usage == HID_USAGE_MOUSE || usage == HID_USAGE_POINTER) {
						return HID_DEV_TYPE_MOUSE;
					}
				}
				else if (collection_type == 1 && usage_page == HID_USAGE_PAGE_DIGITIZER) {
					uint32_t usage = hid_usage_for_index(usages, usage_count,
							usage_range_valid, usage_min, usage_max, 0);
					if (usage == HID_USAGE_TOUCH_SCREEN || usage == HID_USAGE_TOUCH_PAD ||
							usage == HID_USAGE_FINGER) {
						return HID_DEV_TYPE_TOUCH;
					}
				}
			}
			usage_count = 0;
			usage_range_valid = false;
		}
	}
	return HID_DEV_TYPE_UNKNOWN;
}

/* Detect a composite interface (uConsole keyboard: kbd+mouse collections
   share one interrupt endpoint, distinguished by report IDs). Returns 0
   with both IDs filled when the descriptor holds a keyboard AND a
   mouse/pointer application collection each with its own report ID. */
static int hid_parse_report_ids(const uint8_t* desc, int len,
		uint8_t* kbd_id, uint8_t* mouse_id) {
	uint32_t usage_page = 0;
	uint32_t usages[USB_MAX_USAGE_LIST];
	int usage_count = 0;
	int depth = 0;
	hid_dev_type_t cur_app = HID_DEV_TYPE_UNKNOWN;
	bool kbd_found = false, mouse_found = false;

	for (int off = 0; off < len; ) {
		uint8_t prefix = desc[off++];
		uint32_t value = 0;
		int size_code, size, type, tag;

		if (prefix == 0xFE) {
			if (off + 2 > len) break;
			size = desc[off];
			off += 2 + size;
			continue;
		}
		size_code = prefix & 0x3;
		size = (size_code == 3) ? 4 : size_code;
		type = (prefix >> 2) & 0x3;
		tag = (prefix >> 4) & 0xF;
		if (off + size > len) break;
		for (int i = 0; i < size; ++i) {
			value |= (uint32_t)desc[off + i] << (i * 8);
		}
		off += size;

		if (type == 1) { /* global */
			if (tag == 0) {
				usage_page = value;
			}
			else if (tag == 8) { /* Report ID */
				/* only IDs declared inside the collection count: a global
				   Report ID from a preceding collection (consumer/joystick)
				   must not leak into the next one */
				if (cur_app == HID_DEV_TYPE_KEYBOARD && !kbd_found) {
					*kbd_id = (uint8_t)value;
					kbd_found = true;
				}
				else if (cur_app == HID_DEV_TYPE_MOUSE && !mouse_found) {
					*mouse_id = (uint8_t)value;
					mouse_found = true;
				}
			}
		}
		else if (type == 2) { /* local */
			if (tag == 0 && usage_count < USB_MAX_USAGE_LIST) {
				usages[usage_count++] = value;
			}
		}
		else if (type == 0) { /* main */
			if (tag == 10) { /* Collection */
				if (depth == 0 && (uint8_t)value == 1 &&
						usage_page == HID_USAGE_PAGE_GENERIC_DESKTOP && usage_count > 0) {
					if (usages[0] == HID_USAGE_KEYBOARD) {
						cur_app = HID_DEV_TYPE_KEYBOARD;
					}
					else if (usages[0] == HID_USAGE_MOUSE || usages[0] == HID_USAGE_POINTER) {
						cur_app = HID_DEV_TYPE_MOUSE;
					}
					else {
						cur_app = HID_DEV_TYPE_UNKNOWN;
					}
				}
				depth++;
			}
			else if (tag == 12) { /* End Collection */
				if (depth > 0) {
					depth--;
				}
				if (depth == 0) {
					cur_app = HID_DEV_TYPE_UNKNOWN;
				}
			}
			usage_count = 0;
		}
	}
	if (kbd_found && mouse_found && *kbd_id != *mouse_id) {
		return 0;
	}
	return -1;
}

static int hid_parse_touch_report(const uint8_t* desc, int len, touch_parser_t* out) {
	uint32_t usages[USB_MAX_USAGE_LIST];
	int usage_count = 0;
	uint32_t usage_page = 0;
	uint32_t usage_min = 0;
	uint32_t usage_max = 0;
	bool usage_range_valid = false;
	uint32_t report_size = 0;
	uint32_t report_count = 0;
	uint8_t current_report_id = 0;
	uint32_t report_bits[256];
	int collection_depth = 0;
	int touch_collection_depth = -1;
	bool touch_active = false;
	int32_t logical_max = 0;

	memset(report_bits, 0, sizeof(report_bits));
	memset(out, 0, sizeof(*out));
	out->tip_bit = -1;
	out->x_bit = -1;
	out->y_bit = -1;

	for (int off = 0; off < len; ) {
		uint8_t prefix = desc[off++];
		uint32_t value = 0;
		int size_code;
		int size;
		int type;
		int tag;

		if (prefix == 0xFE) {
			if (off + 2 > len) {
				break;
			}
			size = desc[off];
			off += 2;
			off += size;
			continue;
		}

		size_code = prefix & 0x3;
		size = (size_code == 3) ? 4 : size_code;
		type = (prefix >> 2) & 0x3;
		tag = (prefix >> 4) & 0xF;
		if (off + size > len) {
			break;
		}
		for (int i = 0; i < size; ++i) {
			value |= (uint32_t)desc[off + i] << (i * 8);
		}
		off += size;

		if (type == 1) {
			switch (tag) {
			case 0:
				usage_page = value;
				break;
			case 1:
				(void)hid_sign_extend(value, size * 8);
				break;
			case 2:
				logical_max = hid_sign_extend(value, size * 8);
				break;
			case 7:
				report_size = value;
				break;
			case 8:
				current_report_id = (uint8_t)value;
				if (report_bits[current_report_id] == 0) {
					report_bits[current_report_id] = 8;
				}
				break;
			case 9:
				report_count = value;
				break;
			default:
				break;
			}
		}
		else if (type == 2) {
			switch (tag) {
			case 0:
				if (usage_count < USB_MAX_USAGE_LIST) {
					usages[usage_count++] = value;
				}
				break;
			case 1:
				usage_min = value;
				usage_range_valid = true;
				break;
			case 2:
				usage_max = value;
				usage_range_valid = true;
				break;
			default:
				break;
			}
		}
		else if (type == 0) {
			switch (tag) {
			case 8: {
				bool constant = (value & 0x1u) != 0;
				bool variable = (value & 0x2u) != 0;

				if (touch_active && !constant && variable) {
					for (uint32_t idx = 0; idx < report_count; ++idx) {
						uint32_t usage = hid_usage_for_index(usages, usage_count,
								usage_range_valid, usage_min, usage_max, (int)idx);
						int bit = (int)report_bits[current_report_id] + (int)(idx * report_size);

						if (usage_page == HID_USAGE_PAGE_DIGITIZER && usage == HID_USAGE_TIP_SWITCH) {
							if (out->tip_bit < 0) {
								out->tip_bit = bit;
								out->tip_size = (int)report_size;
								out->has_report_id = current_report_id != 0;
								out->report_id = current_report_id;
							}
						}
						else if (usage_page == HID_USAGE_PAGE_GENERIC_DESKTOP && usage == HID_USAGE_X) {
							if (out->x_bit < 0) {
								out->x_bit = bit;
								out->x_size = (int)report_size;
								out->x_max = logical_max > 0 ? (uint32_t)logical_max : 0;
								out->has_report_id = current_report_id != 0;
								out->report_id = current_report_id;
							}
						}
						else if (usage_page == HID_USAGE_PAGE_GENERIC_DESKTOP && usage == HID_USAGE_Y) {
							if (out->y_bit < 0) {
								out->y_bit = bit;
								out->y_size = (int)report_size;
								out->y_max = logical_max > 0 ? (uint32_t)logical_max : 0;
								out->has_report_id = current_report_id != 0;
								out->report_id = current_report_id;
							}
						}
					}
				}
				report_bits[current_report_id] += report_size * report_count;
				clear_local_usages(usages, &usage_count, &usage_range_valid);
				break;
			}
			case 10: {
				uint32_t usage = hid_usage_for_index(usages, usage_count,
						usage_range_valid, usage_min, usage_max, 0);
				uint8_t collection_type = (uint8_t)value;

				if (collection_type == 1 &&
						usage_page == HID_USAGE_PAGE_DIGITIZER &&
						(usage == HID_USAGE_TOUCH_SCREEN ||
						 usage == HID_USAGE_TOUCH_PAD ||
						 usage == HID_USAGE_FINGER)) {
					touch_collection_depth = collection_depth + 1;
					touch_active = true;
				}
				collection_depth++;
				clear_local_usages(usages, &usage_count, &usage_range_valid);
				break;
			}
			case 12:
				if (collection_depth == touch_collection_depth) {
					touch_active = false;
					touch_collection_depth = -1;
				}
				if (collection_depth > 0) {
					collection_depth--;
				}
				clear_local_usages(usages, &usage_count, &usage_range_valid);
				break;
			default:
				clear_local_usages(usages, &usage_count, &usage_range_valid);
				break;
			}
		}
	}

	if (out->tip_bit < 0 || out->x_bit < 0 || out->y_bit < 0) {
		return -1;
	}

	out->valid = true;
	out->report_bytes = (uint8_t)((report_bits[out->report_id] + 7u) / 8u);
	if (out->report_bytes == 0 || out->report_bytes > USB_MAX_REPORT) {
		return -1;
	}
	return 0;
}

static uint32_t bit_extract_le(const uint8_t* buf, int bit, int bits) {
	uint32_t value = 0;
	for (int i = 0; i < bits; ++i) {
		int off = bit + i;
		if ((buf[off / 8] & (1u << (off % 8))) != 0) {
			value |= 1u << i;
		}
	}
	return value;
}

static int touch_normalize_report(const usb_input_dev_t* in, const uint8_t* report, int len, uint8_t* out) {
	bool pressed;
	uint32_t x;
	uint32_t y;

	if (!in->touch.valid) {
		return -1;
	}
	if (in->touch.has_report_id) {
		if (len <= 0 || report[0] != in->touch.report_id) {
			return -1;
		}
	}
	if (len < in->report_len) {
		return -1;
	}

	pressed = bit_extract_le(report, in->touch.tip_bit, in->touch.tip_size) != 0;
	x = bit_extract_le(report, in->touch.x_bit, in->touch.x_size);
	y = bit_extract_le(report, in->touch.y_bit, in->touch.y_size);
	if (x > 0xFFFFu) {
		x = 0xFFFFu;
	}
	if (y > 0xFFFFu) {
		y = 0xFFFFu;
	}

	out[0] = pressed ? 1 : 0;
	out[1] = (uint8_t)(x & 0xFFu);
	out[2] = (uint8_t)((x >> 8) & 0xFFu);
	out[3] = (uint8_t)(y & 0xFFu);
	out[4] = (uint8_t)((y >> 8) & 0xFFu);
	out[5] = 0;
	out[6] = 0;
	return USB_EVENT_SIZE;
}

static void usb_inputs_clear(void) {
	memset(_inputs, 0, sizeof(_inputs));
}

static int usb_input_alloc(void) {
	for (int i = 0; i < USB_MAX_INPUTS; ++i) {
		if (!_inputs[i].present) {
			return i;
		}
	}
	return -1;
}

static int usb_parse_candidates(const uint8_t* cfg, int cfg_len, hid_candidate_t* candidates, int max_candidates) {
	const usb_iface_desc_t* current_iface = NULL;
	uint16_t current_report_desc_len = 0;
	int count = 0;

	for (int off = 0; off + 2 <= cfg_len; ) {
		uint8_t len = cfg[off];
		uint8_t type = cfg[off + 1];

		if (len < 2 || off + len > cfg_len) {
			break;
		}

		if (type == USB_DESC_INTERFACE && len >= sizeof(usb_iface_desc_t)) {
			current_iface = (const usb_iface_desc_t*)(cfg + off);
			current_report_desc_len = 0;
		}
		else if (type == USB_DESC_HID && current_iface != NULL && len >= sizeof(usb_hid_desc_t)) {
			const usb_hid_desc_t* hid = (const usb_hid_desc_t*)(cfg + off);
			current_report_desc_len = hid->wReportDescriptorLength;
		}
		else if (type == USB_DESC_ENDPOINT &&
				current_iface != NULL &&
				len >= sizeof(usb_ep_desc_t) &&
				current_iface->bInterfaceClass == USB_CLASS_HID) {
			const usb_ep_desc_t* ep = (const usb_ep_desc_t*)(cfg + off);

			if ((ep->bEndpointAddress & USB_ENDPOINT_IN) != 0 &&
					(ep->bmAttributes & 0x3u) == USB_ENDPOINT_XFER_INTERRUPT &&
					count < max_candidates) {
				candidates[count].valid = true;
				candidates[count].iface_num = current_iface->bInterfaceNumber;
				candidates[count].subclass = current_iface->bInterfaceSubClass;
				candidates[count].protocol = current_iface->bInterfaceProtocol;
				candidates[count].ep_addr = ep->bEndpointAddress;
				{
					/* clamp aggressive bInterval (1-4ms on gaming mice) to the
					   poll floor: every poll is a costly channel round-trip */
					uint8_t iv = ep->bInterval == 0 ? 10 : ep->bInterval;
					if (iv < USB_POLL_INTERVAL_MIN_MS) {
						iv = USB_POLL_INTERVAL_MIN_MS;
					}
					candidates[count].interval = iv;
				}
				candidates[count].max_packet = (uint16_t)(ep->wMaxPacketSize & 0x07FFu);
				candidates[count].report_desc_len = current_report_desc_len;
				count++;
			}
		}

		off += len;
	}
	return count;
}

static int usb_register_keyboard(uint8_t addr, bool low_speed, uint8_t ctrl_mps, const hid_candidate_t* cand) {
	int slot = usb_input_alloc();
	if (slot < 0) {
		slog("usbhostd: register keyboard failed no_slot addr=%u iface=%u\n", addr, cand->iface_num);
		return -1;
	}
	/* Set_Protocol is only defined for boot-subclass interfaces; non-boot
	   interfaces default to Report protocol and must not receive it. */
	if (cand->subclass == USB_SUBCLASS_BOOT) {
		(void)usb_hid_set_protocol(addr, low_speed, ctrl_mps, cand->iface_num, 0);
	}
	(void)usb_hid_set_idle(addr, low_speed, ctrl_mps, cand->iface_num);
	memset(&_inputs[slot], 0, sizeof(_inputs[slot]));
	_inputs[slot].present = true;
	_inputs[slot].type = USB_INPUT_KEYBOARD;
	_inputs[slot].addr = addr;
	_inputs[slot].iface_num = cand->iface_num;
	_inputs[slot].ep_addr = cand->ep_addr;
	_inputs[slot].interval = cand->interval;
	_inputs[slot].max_packet = cand->max_packet == 0 ? 8 : cand->max_packet;
	_inputs[slot].ctrl_mps = ctrl_mps;
	_inputs[slot].low_speed = low_speed;
	_inputs[slot].report_len = 8;
	slog("usbhostd: register keyboard slot=%d addr=%u iface=%u ep=%02x interval=%u maxpkt=%u speed=%s subclass=%u\n",
			slot, addr, cand->iface_num, cand->ep_addr, cand->interval,
			cand->max_packet, usb_speed_name(low_speed), cand->subclass);
	return 0;
}

static int usb_register_mouse(uint8_t addr, bool low_speed, uint8_t ctrl_mps, const hid_candidate_t* cand) {
	int slot = usb_input_alloc();
	if (slot < 0) {
		slog("usbhostd: register mouse failed no_slot addr=%u iface=%u\n", addr, cand->iface_num);
		return -1;
	}
	/* Set_Protocol is only defined for boot-subclass interfaces; non-boot
	   interfaces default to Report protocol and must not receive it. */
	if (cand->subclass == USB_SUBCLASS_BOOT) {
		(void)usb_hid_set_protocol(addr, low_speed, ctrl_mps, cand->iface_num, 0);
	}
	(void)usb_hid_set_idle(addr, low_speed, ctrl_mps, cand->iface_num);
	memset(&_inputs[slot], 0, sizeof(_inputs[slot]));
	_inputs[slot].present = true;
	_inputs[slot].type = USB_INPUT_MOUSE;
	_inputs[slot].addr = addr;
	_inputs[slot].iface_num = cand->iface_num;
	_inputs[slot].ep_addr = cand->ep_addr;
	_inputs[slot].interval = cand->interval;
	_inputs[slot].max_packet = cand->max_packet == 0 ? 8 : cand->max_packet;
	_inputs[slot].ctrl_mps = ctrl_mps;
	_inputs[slot].low_speed = low_speed;
	_inputs[slot].report_len = cand->max_packet > 0 ? cand->max_packet : 4;
	slog("usbhostd: register mouse slot=%d addr=%u iface=%u ep=%02x interval=%u maxpkt=%u speed=%s subclass=%u\n",
			slot, addr, cand->iface_num, cand->ep_addr, cand->interval,
			cand->max_packet, usb_speed_name(low_speed), cand->subclass);
	return 0;
}

static int usb_register_composite(uint8_t addr, bool low_speed, uint8_t ctrl_mps,
		const hid_candidate_t* cand, uint8_t kbd_id, uint8_t mouse_id) {
	int slot = usb_input_alloc();
	if (slot < 0) {
		slog("usbhostd: register composite failed no_slot addr=%u iface=%u\n", addr, cand->iface_num);
		return -1;
	}
	/* report IDs only exist in Report protocol; boot protocol would strip
	   the mouse collection entirely */
	if (cand->subclass == USB_SUBCLASS_BOOT) {
		(void)usb_hid_set_protocol(addr, low_speed, ctrl_mps, cand->iface_num, 1);
	}
	(void)usb_hid_set_idle(addr, low_speed, ctrl_mps, cand->iface_num);
	memset(&_inputs[slot], 0, sizeof(_inputs[slot]));
	_inputs[slot].present = true;
	_inputs[slot].type = USB_INPUT_COMPOSITE;
	_inputs[slot].addr = addr;
	_inputs[slot].iface_num = cand->iface_num;
	_inputs[slot].ep_addr = cand->ep_addr;
	_inputs[slot].interval = cand->interval;
	_inputs[slot].max_packet = cand->max_packet == 0 ? 8 : cand->max_packet;
	_inputs[slot].ctrl_mps = ctrl_mps;
	_inputs[slot].low_speed = low_speed;
	/* variable-size reports: always request a full packet */
	_inputs[slot].report_len = _inputs[slot].max_packet > USB_MAX_REPORT ?
			USB_MAX_REPORT : (uint8_t)_inputs[slot].max_packet;
	_inputs[slot].kbd_report_id = kbd_id;
	_inputs[slot].mouse_report_id = mouse_id;
	slog("usbhostd: register composite slot=%d addr=%u iface=%u ep=%02x interval=%u maxpkt=%u kbd_id=%u mouse_id=%u speed=%s subclass=%u\n",
			slot, addr, cand->iface_num, cand->ep_addr, cand->interval,
			cand->max_packet, kbd_id, mouse_id, usb_speed_name(low_speed), cand->subclass);
	return 0;
}

static int usb_register_touch(uint8_t addr, bool low_speed, uint8_t ctrl_mps, const hid_candidate_t* cand) {
	int slot;
	uint8_t* report_desc;
	touch_parser_t parser;

	if (cand->report_desc_len == 0 || cand->report_desc_len > 1024) {
		slog("usbhostd: register touch skip invalid_report_desc addr=%u iface=%u len=%u\n",
				addr, cand->iface_num, cand->report_desc_len);
		return -1;
	}

	report_desc = (uint8_t*)malloc(cand->report_desc_len);
	if (report_desc == NULL) {
		slog("usbhostd: register touch alloc_failed addr=%u iface=%u len=%u\n",
				addr, cand->iface_num, cand->report_desc_len);
		return -1;
	}

	if (usb_get_descriptor(addr, low_speed, ctrl_mps, USB_REQTYPE_STD_IFACE_IN,
			USB_DESC_HID_REPORT, 0, cand->iface_num, report_desc, cand->report_desc_len) < cand->report_desc_len) {
		slog("usbhostd: register touch get_report_desc_failed addr=%u iface=%u len=%u\n",
				addr, cand->iface_num, cand->report_desc_len);
		free(report_desc);
		return -1;
	}
	if (hid_parse_touch_report(report_desc, cand->report_desc_len, &parser) != 0) {
		slog("usbhostd: register touch parse_failed addr=%u iface=%u len=%u\n",
				addr, cand->iface_num, cand->report_desc_len);
		free(report_desc);
		return -1;
	}
	free(report_desc);

	slot = usb_input_alloc();
	if (slot < 0) {
		slog("usbhostd: register touch failed no_slot addr=%u iface=%u\n", addr, cand->iface_num);
		return -1;
	}

	memset(&_inputs[slot], 0, sizeof(_inputs[slot]));
	_inputs[slot].present = true;
	_inputs[slot].type = USB_INPUT_TOUCH;
	_inputs[slot].addr = addr;
	_inputs[slot].iface_num = cand->iface_num;
	_inputs[slot].ep_addr = cand->ep_addr;
	_inputs[slot].interval = cand->interval;
	_inputs[slot].max_packet = cand->max_packet == 0 ? parser.report_bytes : cand->max_packet;
	_inputs[slot].ctrl_mps = ctrl_mps;
	_inputs[slot].low_speed = low_speed;
	_inputs[slot].report_len = parser.report_bytes;
	_inputs[slot].touch = parser;
	slog("usbhostd: register touch slot=%d addr=%u iface=%u ep=%02x interval=%u maxpkt=%u report_id=%u report_len=%u tip=%d x=%d y=%d\n",
			slot, addr, cand->iface_num, cand->ep_addr, cand->interval,
			cand->max_packet, parser.report_id, parser.report_bytes,
			parser.tip_bit, parser.x_bit, parser.y_bit);
	return 0;
}

static int usb_enumerate_device(bool low_speed, int depth);

/* ---- hub class support: the uConsole keyboard sits behind a GL850G hub ---- */

static int usb_hub_port_status(uint8_t addr, bool low_speed, uint8_t ep_mps,
		uint8_t port, uint16_t* status, uint16_t* change) {
	usb_setup_pkt_t setup;
	uint8_t buf[4];
	memset(&setup, 0, sizeof(setup));
	setup.bmRequestType = USB_REQTYPE_CLASS_PORT_IN;
	setup.bRequest = USB_REQ_GET_STATUS;
	setup.wIndex = port;
	setup.wLength = 4;
	if (usb_control_msg(addr, low_speed, ep_mps, &setup, buf, true) < 4) {
		return -1;
	}
	if (status != NULL) {
		*status = le16(buf);
	}
	if (change != NULL) {
		*change = le16(buf + 2);
	}
	return 0;
}

static int usb_hub_port_feature(uint8_t addr, bool low_speed, uint8_t ep_mps,
		uint8_t port, uint16_t feature, bool set) {
	usb_setup_pkt_t setup;
	memset(&setup, 0, sizeof(setup));
	setup.bmRequestType = USB_REQTYPE_CLASS_PORT_OUT;
	setup.bRequest = set ? USB_REQ_SET_FEATURE : USB_REQ_CLEAR_FEATURE;
	setup.wValue = feature;
	setup.wIndex = port;
	return usb_control_msg(addr, low_speed, ep_mps, &setup, NULL, false);
}

static int usb_enumerate_hub(uint8_t addr, bool low_speed, uint8_t ep_mps, int depth) {
	usb_setup_pkt_t setup;
	uint8_t hub_desc[9];
	uint8_t num_ports;
	uint32_t pwr_ms;
	int registered = 0;

	memset(&setup, 0, sizeof(setup));
	setup.bmRequestType = USB_REQTYPE_CLASS_DEV_IN;
	setup.bRequest = USB_REQ_GET_DESCRIPTOR;
	setup.wValue = (uint16_t)(USB_DESC_HUB << 8);
	setup.wLength = sizeof(hub_desc);
	if (usb_control_msg(addr, low_speed, ep_mps, &setup, hub_desc, true) < (int)sizeof(hub_desc)) {
		slog("usbhostd: hub addr=%u get_hub_desc_failed\n", addr);
		return -1;
	}
	num_ports = hub_desc[2];
	/* bPwrOn2PwrGood is in 2ms units; add margin for slow rails */
	pwr_ms = (uint32_t)hub_desc[5] * 2u + 100u;
	slog("usbhostd: hub addr=%u ports=%u pwr2good=%ums depth=%d\n",
			addr, num_ports, pwr_ms, depth);
	if (num_ports > 8) {
		num_ports = 8;
	}

	for (uint8_t port = 1; port <= num_ports; ++port) {
		usb_hub_port_feature(addr, low_speed, ep_mps, port, USB_HUB_FEAT_PORT_POWER, true);
	}
	proc_usleep(pwr_ms * 1000u);

	for (uint8_t port = 1; port <= num_ports; ++port) {
		uint16_t status = 0;
		uint16_t change = 0;
		int attempt;
		int ret = -1;

		if (usb_hub_port_status(addr, low_speed, ep_mps, port, &status, &change) != 0) {
			slog("usbhostd: hub addr=%u port=%u status_failed\n", addr, port);
			continue;
		}
		if (USB_LOG_RUNTIME_VERBOSE) {
			slog("usbhostd: hub addr=%u port=%u status=%04x change=%04x\n",
					addr, port, status, change);
		}
		if ((status & USB_HUB_PS_CONNECTION) == 0) {
			continue;
		}
		usb_hub_port_feature(addr, low_speed, ep_mps, port,
				USB_HUB_FEAT_C_PORT_CONNECTION, false);

		/* freshly reset devices (uConsole keyboard MCU) may answer the first
		   SETUPs with transaction errors: re-reset the port and retry */
		for (attempt = 1; attempt <= 3 && ret <= 0; ++attempt) {
			bool enabled = false;
			int waited;

			if (usb_hub_port_feature(addr, low_speed, ep_mps, port,
					USB_HUB_FEAT_PORT_RESET, true) < 0) {
				slog("usbhostd: hub addr=%u port=%u reset_req_failed attempt=%d\n",
						addr, port, attempt);
				continue;
			}
			for (waited = 0; waited < 25; ++waited) {
				proc_usleep(20000);
				if (usb_hub_port_status(addr, low_speed, ep_mps, port, &status, &change) != 0) {
					break;
				}
				if ((status & USB_HUB_PS_RESET) == 0 && (status & USB_HUB_PS_ENABLE) != 0) {
					enabled = true;
					break;
				}
			}
			usb_hub_port_feature(addr, low_speed, ep_mps, port,
					USB_HUB_FEAT_C_PORT_RESET, false);
			if (!enabled) {
				slog("usbhostd: hub addr=%u port=%u reset_failed status=%04x attempt=%d\n",
						addr, port, status, attempt);
				proc_usleep(100000);
				continue;
			}
			/* reset recovery: give the device MCU time to come alive */
			proc_usleep(60000);
			slog("usbhostd: hub addr=%u port=%u connected speed=%s attempt=%d\n", addr, port,
					usb_speed_name((status & USB_HUB_PS_LOW_SPEED) != 0), attempt);
			ret = usb_enumerate_device((status & USB_HUB_PS_LOW_SPEED) != 0, depth + 1);
			if (ret <= 0 && attempt < 3) {
				if (USB_LOG_RUNTIME_VERBOSE) {
					slog("usbhostd: hub addr=%u port=%u enum_retry attempt=%d\n",
							addr, port, attempt + 1);
				}
				proc_usleep(100000);
			}
		}
		if (ret > 0) {
			registered += ret;
		}
	}
	return registered;
}

/* enumerate the device currently at default address 0 (fresh after port reset) */
static int usb_enumerate_device(bool low_speed, int depth) {
	uint8_t desc8[8];
	usb_device_desc_t dev_desc;
	uint8_t cfg_head[9];
	uint8_t* cfg_buf = NULL;
	uint8_t addr;
	uint8_t ctrl_mps;
	uint16_t total_len;
	int cand_count;
	hid_candidate_t candidates[USB_MAX_CANDIDATES];
	int registered = 0;

	if (usb_get_descriptor(0, low_speed, 8, USB_REQTYPE_STD_IN,
			USB_DESC_DEVICE, 0, 0, desc8, sizeof(desc8)) < 8) {
		if (USB_LOG_RUNTIME_VERBOSE) {
			slog("usbhostd: enumerate dev get_device_desc8_failed depth=%d\n", depth);
		}
		return -1;
	}

	addr = _next_address++;
	if (addr == 0 || addr > 126) {
		_next_address = 2;
		addr = _next_address++;
	}
	if (usb_set_address(low_speed, addr) < 0) {
		slog("usbhostd: enumerate dev set_address_failed addr=%u\n", addr);
		return -1;
	}
	proc_usleep(10000);

	ctrl_mps = desc8[7];
	memset(&dev_desc, 0, sizeof(dev_desc));
	if (usb_get_descriptor(addr, low_speed, ctrl_mps, USB_REQTYPE_STD_IN,
			USB_DESC_DEVICE, 0, 0, &dev_desc, sizeof(dev_desc)) < (int)sizeof(dev_desc)) {
		slog("usbhostd: enumerate dev get_device_desc_failed addr=%u\n", addr);
		return -1;
	}
	ctrl_mps = dev_desc.bMaxPacketSize0;
	slog("usbhostd: enumerate dev addr=%u vid=%04x pid=%04x class=%02x subclass=%02x proto=%02x mps0=%u speed=%s depth=%d\n",
			addr, dev_desc.idVendor, dev_desc.idProduct, dev_desc.bDeviceClass,
			dev_desc.bDeviceSubClass, dev_desc.bDeviceProtocol, ctrl_mps,
			usb_speed_name(low_speed), depth);

	if (usb_get_descriptor(addr, low_speed, ctrl_mps, USB_REQTYPE_STD_IN,
			USB_DESC_CONFIG, 0, 0, cfg_head, sizeof(cfg_head)) < (int)sizeof(cfg_head)) {
		slog("usbhostd: enumerate dev get_config_head_failed addr=%u\n", addr);
		return -1;
	}

	total_len = le16(cfg_head + 2);
	if (total_len < sizeof(usb_config_desc_t) || total_len > 1024) {
		slog("usbhostd: enumerate dev invalid_config_len addr=%u total=%u\n", addr, total_len);
		return -1;
	}

	cfg_buf = (uint8_t*)malloc(total_len);
	if (cfg_buf == NULL) {
		slog("usbhostd: enumerate dev alloc_config_failed addr=%u total=%u\n", addr, total_len);
		return -1;
	}
	if (usb_get_descriptor(addr, low_speed, ctrl_mps, USB_REQTYPE_STD_IN,
			USB_DESC_CONFIG, 0, 0, cfg_buf, total_len) < total_len) {
		slog("usbhostd: enumerate dev get_config_failed addr=%u total=%u\n", addr, total_len);
		free(cfg_buf);
		return -1;
	}

	if (usb_set_configuration(addr, low_speed, ctrl_mps, ((usb_config_desc_t*)cfg_buf)->bConfigurationValue) < 0) {
		slog("usbhostd: enumerate dev set_config_failed addr=%u cfg=%u\n",
				addr, ((usb_config_desc_t*)cfg_buf)->bConfigurationValue);
		free(cfg_buf);
		return -1;
	}
	proc_usleep(10000);

	if (dev_desc.bDeviceClass == USB_CLASS_HUB) {
		free(cfg_buf);
		if (depth >= 2) {
			slog("usbhostd: enumerate dev hub_too_deep addr=%u depth=%d\n", addr, depth);
			return 0;
		}
		return usb_enumerate_hub(addr, low_speed, ctrl_mps, depth);
	}

	memset(candidates, 0, sizeof(candidates));
	cand_count = usb_parse_candidates(cfg_buf, total_len, candidates, USB_MAX_CANDIDATES);
	slog("usbhostd: enumerate dev candidates=%d config_len=%u addr=%u\n", cand_count, total_len, addr);
	free(cfg_buf);

	for (int i = 0; i < cand_count; ++i) {
		if (!candidates[i].valid) {
			continue;
		}
		slog("usbhostd: candidate idx=%d iface=%u subclass=%u proto=%u ep=%02x interval=%u maxpkt=%u report_desc=%u\n",
				i, candidates[i].iface_num, candidates[i].subclass, candidates[i].protocol,
				candidates[i].ep_addr, candidates[i].interval, candidates[i].max_packet,
				candidates[i].report_desc_len);
		{
			uint8_t* report_desc = NULL;
			bool desc_ok = false;
			uint8_t kbd_id = 0, mouse_id = 0;

			if (candidates[i].report_desc_len > 0 && candidates[i].report_desc_len <= 1024) {
				report_desc = (uint8_t*)malloc(candidates[i].report_desc_len);
				if (report_desc != NULL &&
						usb_get_descriptor(addr, low_speed, ctrl_mps, USB_REQTYPE_STD_IFACE_IN,
							USB_DESC_HID_REPORT, 0, candidates[i].iface_num, report_desc,
							candidates[i].report_desc_len) >= candidates[i].report_desc_len) {
					desc_ok = true;
				}
			}

			/* uConsole keyboard: boot-keyboard interface whose report
			   descriptor actually multiplexes kbd+mouse via report IDs */
			if (desc_ok && hid_parse_report_ids(report_desc,
					candidates[i].report_desc_len, &kbd_id, &mouse_id) == 0) {
				if (usb_register_composite(addr, low_speed, ctrl_mps, &candidates[i],
						kbd_id, mouse_id) == 0) {
					registered++;
				}
			}
			else if (candidates[i].subclass == USB_SUBCLASS_BOOT &&
					candidates[i].protocol == USB_PROTOCOL_KEYBOARD) {
				if (usb_register_keyboard(addr, low_speed, ctrl_mps, &candidates[i]) == 0) {
					registered++;
				}
			}
			else if (candidates[i].subclass == USB_SUBCLASS_BOOT &&
					candidates[i].protocol == USB_PROTOCOL_MOUSE) {
				if (usb_register_mouse(addr, low_speed, ctrl_mps, &candidates[i]) == 0) {
					registered++;
				}
			}
			else {
				hid_dev_type_t dev_type = HID_DEV_TYPE_UNKNOWN;

				if (desc_ok) {
					dev_type = hid_detect_device_type(report_desc, candidates[i].report_desc_len);
					slog("usbhostd: candidate idx=%d report_desc_type=%d\n", i, dev_type);
				}

				if (dev_type == HID_DEV_TYPE_KEYBOARD) {
					if (usb_register_keyboard(addr, low_speed, ctrl_mps, &candidates[i]) == 0) {
						registered++;
					}
				}
				else if (dev_type == HID_DEV_TYPE_MOUSE) {
					if (usb_register_mouse(addr, low_speed, ctrl_mps, &candidates[i]) == 0) {
						registered++;
					}
				}
				else if (dev_type == HID_DEV_TYPE_TOUCH) {
					if (usb_register_touch(addr, low_speed, ctrl_mps, &candidates[i]) == 0) {
						registered++;
					}
				}
				else {
					slog("usbhostd: candidate idx=%d unknown_type skip\n", i);
				}
			}

			if (report_desc != NULL) {
				free(report_desc);
			}
		}
	}

	slog("usbhostd: enumerate dev done addr=%u registered=%d depth=%d\n",
			addr, registered, depth);
	return registered;
}

static int usb_enumerate_root(void) {
	bool low_speed = false;
	int registered;
	uint64_t enum_start_ms = kernel_tic_ms(0);

	if (USB_LOG_RUNTIME_VERBOSE) {
		slog("usbhostd: enumerate root begin\n");
	}
	usb_inputs_clear();
	_next_address = 2;
	if (dwc_reset_port(&low_speed) != 0) {
		if (USB_LOG_RUNTIME_VERBOSE) {
			slog("usbhostd: enumerate root reset_failed\n");
		}
		return -1;
	}

	registered = usb_enumerate_device(low_speed, 0);

	slog("usbhostd: enumerate root done registered=%d cost=%ums\n",
			registered, (uint32_t)(kernel_tic_ms(0) - enum_start_ms));
	if (registered > 0) {
		_stats.enum_ok++;
	}
	else {
		_stats.enum_fail++;
	}
	return registered > 0 ? registered : -1;
}

static void usb_scan_root(void) {
	bool connected = dwc_port_connected();

	if (!connected) {
		if (_device_ready) {
			_stats.port_disconnect++;
			slog("usbhostd: port disconnected, clear active inputs hprt=%08x gintsts=%08x\n",
					usb_readl(DWC_REG_HPRT), usb_readl(DWC_REG_GINTSTS));
			usb_inputs_clear();
			_device_ready = false;
		}
		else if (_port_connected) {
			_stats.port_disconnect++;
			slog("usbhostd: port disconnected before ready hprt=%08x\n",
					usb_readl(DWC_REG_HPRT));
		}
		else {
			/* nothing attached: periodically dump port/OTG state so the
			   electrical situation is visible (conid=B means the ID pin
			   claims device role: the HID device is likely on another
			   controller or VBUS is not reaching it) */
			uint64_t now = kernel_tic_ms(0);
			if (USB_LOG_RUNTIME_VERBOSE && now >= _next_idle_log_ms) {
				uint32_t gotg = usb_readl(DWC_REG_GOTGCTL);
				uint32_t gint = usb_readl(DWC_REG_GINTSTS);
				_next_idle_log_ms = now + 5000u;
				slog("usbhostd: port idle no_connect hprt=%08x gotgctl=%08x conid=%s asesvld=%u bsesvld=%u curmod=%s gintsts=%08x hfnum=%04x\n",
						usb_readl(DWC_REG_HPRT), gotg,
						(gotg & DWC_GOTGCTL_CONID_B) ? "B" : "A",
						(gotg & DWC_GOTGCTL_ASESVLD) ? 1u : 0u,
						(gotg & DWC_GOTGCTL_BSESVLD) ? 1u : 0u,
						(gint & DWC_GINTSTS_CURMODE_HOST) ? "host" : "device",
						gint, usb_readl(DWC_REG_HFNUM) & 0xFFFFu);
			}
		}
		_port_connected = false;
		return;
	}

	if (!_port_connected) {
		slog("usbhostd: port connected hprt=%08x\n", usb_readl(DWC_REG_HPRT));
	}
	_port_connected = true;
	if (!_device_ready) {
		int found = usb_enumerate_root();
		if (found > 0) {
			_device_ready = true;
			_enum_fail_streak = 0;
			slog("usbhostd: device ready inputs=%d\n", found);
		}
		else {
			if (USB_LOG_RUNTIME_VERBOSE) {
				slog("usbhostd: enumerate root pending\n");
			}
			_enum_fail_streak++;
			/* back off before re-resetting the port: hammering resets
			   destabilizes hubs (GL850G shows not_enabled/setup errors) */
			_next_scan_ms = kernel_tic_ms(0) + 2000u;
			/* the dwc2 port can latch a dead not_enabled/EnaChng state that
			   only a full core re-init clears */
			if (_enum_fail_streak >= 3) {
				slog("usbhostd: core reinit after %u failed enumerations\n",
						_enum_fail_streak);
				_enum_fail_streak = 0;
				if (dwc_host_init() != 0) {
					slog("usbhostd: core reinit failed\n");
				}
				_next_scan_ms = kernel_tic_ms(0) + 1000u;
			}
		}
	}
	dwc_ack_port_change();
}

/* effective poll cadence for an input endpoint: base bInterval while data
   flows, stretched up to 4x (capped) once the endpoint keeps NAKing, and
   snapped back to base by the first report that carries data */
static uint32_t usb_input_poll_interval(const usb_input_dev_t* in) {
	uint32_t iv = in->interval == 0 ? 10u : in->interval;

	if (in->idle_polls >= USB_POLL_IDLE_THRESHOLD * 4u) {
		iv *= 4u;
	}
	else if (in->idle_polls >= USB_POLL_IDLE_THRESHOLD) {
		iv *= 2u;
	}
	if (iv > USB_POLL_INTERVAL_MAX_MS) {
		iv = USB_POLL_INTERVAL_MAX_MS;
	}
	return iv;
}

static void usb_poll_inputs(vdevice_t* dev) {
	uint64_t now = kernel_tic_ms(0);
	uint8_t report[USB_MAX_REPORT];
	uint8_t payload[USB_EVENT_SIZE];
	bool wakeup = false;

	for (int i = 0; i < USB_MAX_INPUTS; ++i) {
		usb_input_dev_t* in = &_inputs[i];
		int ret;

		if (!in->present) {
			continue;
		}
		if (now < in->next_poll_ms) {
			continue;
		}
		in->next_poll_ms = now + usb_input_poll_interval(in);

		memset(report, 0, sizeof(report));
		ret = usb_interrupt_in(in, report, in->report_len == 0 ? in->max_packet : in->report_len);
		if (ret < 0) {
			in->poll_fail_total++;
			in->poll_fail_streak++;
			in->idle_polls++;
			/* log the 1st failure and then every 100th to track flaky endpoints
			   without flooding the log */
			if (USB_LOG_RUNTIME_VERBOSE &&
					(in->poll_fail_streak == 1 || (in->poll_fail_streak % 100u) == 0)) {
				slog("usbhostd: poll fail type=%s addr=%u ep=%02x streak=%u total=%u toggle=%u hprt=%08x\n",
						usb_input_type_name(in->type), in->addr, in->ep_addr,
						in->poll_fail_streak, in->poll_fail_total, in->toggle,
						usb_readl(DWC_REG_HPRT));
			}
			continue;
		}
		if (in->poll_fail_streak > 0) {
			if (USB_LOG_RUNTIME_VERBOSE) {
				slog("usbhostd: poll recovered type=%s addr=%u ep=%02x after=%u fails\n",
						usb_input_type_name(in->type), in->addr, in->ep_addr,
						in->poll_fail_streak);
			}
			in->poll_fail_streak = 0;
		}
		if (ret == 0) {
			in->idle_polls++;
			continue;
		}
		/* real data arrived: restore the base poll cadence immediately so
		   an active mouse/keyboard is sampled at full rate again (the slot
		   above was armed with the stretched idle interval) */
		in->idle_polls = 0;
		in->next_poll_ms = now + usb_input_poll_interval(in);

		if (in->type == USB_INPUT_KEYBOARD) {
			if ((uint8_t)ret == in->last_len && memcmp(in->last_report, report, ret) == 0) {
				continue;
			}
			memcpy(in->last_report, report, ret);
			in->last_len = (uint8_t)ret;
			memset(payload, 0, sizeof(payload));
			memcpy(payload, report, ret > USB_EVENT_SIZE ? USB_EVENT_SIZE : ret);
			dispatch_data(USB_REPORT_ID_KEYBOARD, payload, USB_EVENT_SIZE);
			usb_log_input_event(in, report, ret, payload);
			wakeup = true;
		}
		else if (in->type == USB_INPUT_MOUSE) {
			memset(payload, 0, sizeof(payload));
			memcpy(payload, report, ret > USB_EVENT_SIZE ? USB_EVENT_SIZE : ret);
			dispatch_data(USB_REPORT_ID_MOUSE, payload, USB_EVENT_SIZE);
			usb_log_input_event(in, report, ret, payload);
			wakeup = true;
		}
		else if (in->type == USB_INPUT_TOUCH) {
			if ((uint8_t)ret == in->last_len && memcmp(in->last_report, report, ret) == 0) {
				continue;
			}
			memcpy(in->last_report, report, ret);
			in->last_len = (uint8_t)ret;
			if (touch_normalize_report(in, report, ret, payload) == USB_EVENT_SIZE) {
				dispatch_data(USB_REPORT_ID_TOUCH, payload, USB_EVENT_SIZE);
				usb_log_input_event(in, report, ret, payload);
				wakeup = true;
			}
		}
		else if (in->type == USB_INPUT_COMPOSITE) {
			/* first byte is the HID report ID; strip it and route */
			uint8_t rid = report[0];
			if (ret < 2) {
				continue;
			}
			if (rid == in->kbd_report_id) {
				if ((uint8_t)ret == in->last_len && memcmp(in->last_report, report, ret) == 0) {
					continue;
				}
				memcpy(in->last_report, report, ret);
				in->last_len = (uint8_t)ret;
				memset(payload, 0, sizeof(payload));
				memcpy(payload, report + 1, (ret - 1) > USB_EVENT_SIZE ? USB_EVENT_SIZE : (ret - 1));
				dispatch_data(USB_REPORT_ID_KEYBOARD, payload, USB_EVENT_SIZE);
				usb_log_input_event(in, report, ret, payload);
				wakeup = true;
			}
			else if (rid == in->mouse_report_id) {
				memset(payload, 0, sizeof(payload));
				memcpy(payload, report + 1, (ret - 1) > USB_EVENT_SIZE ? USB_EVENT_SIZE : (ret - 1));
				dispatch_data(USB_REPORT_ID_MOUSE, payload, USB_EVENT_SIZE);
				usb_log_input_event(in, report, ret, payload);
				wakeup = true;
			}
			/* other report IDs (gamepad etc.): no consumer yet, drop silently */
		}
	}

	if (wakeup) {
		vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
	}
}

static uint32_t usb_step_sleep_us(void) {
	uint64_t now = kernel_tic_ms(0);
	uint64_t next_ms = now + (USB_IDLE_SLEEP_MAX_US / 1000u);

	if (_next_scan_ms <= now) {
		return USB_IDLE_SLEEP_MIN_US;
	}
	if (_next_scan_ms < next_ms) {
		next_ms = _next_scan_ms;
	}

	if (_device_ready) {
		for (int i = 0; i < USB_MAX_INPUTS; ++i) {
			usb_input_dev_t* in = &_inputs[i];

			if (!in->present) {
				continue;
			}
			if (in->next_poll_ms <= now) {
				return USB_IDLE_SLEEP_MIN_US;
			}
			if (in->next_poll_ms < next_ms) {
				next_ms = in->next_poll_ms;
			}
		}
	}

	if (next_ms <= now) {
		return USB_IDLE_SLEEP_MIN_US;
	}

	return (uint32_t)((next_ms - now) * 1000u);
}

static int usb_step(vdevice_t* dev, void* p) {
	uint64_t now = kernel_tic_ms(0);
	(void)p;
	if (now >= _next_scan_ms) {
		_next_scan_ms = 0;
		usb_scan_root();
		/* usb_scan_root may set a longer backoff after a failed enumeration */
		if (_next_scan_ms < kernel_tic_ms(0) + USB_SCAN_INTERVAL_MS) {
			_next_scan_ms = kernel_tic_ms(0) + USB_SCAN_INTERVAL_MS;
		}
	}
	if (_device_ready) {
		usb_poll_inputs(dev);
	}
	usb_stats_log();
	proc_usleep(usb_step_sleep_us());
	return 0;
}

static int usb_open(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node, int oflag, void* p) {
	fd_info_t* info;
	(void)dev;
	(void)node;
	(void)oflag;
	(void)p;
	if (fd < 0) {
		return -1;
	}
	info = (fd_info_t*)calloc(1, sizeof(fd_info_t));
	if (info == NULL) {
		return -1;
	}
	info->fd = fd;
	info->from_pid = from_pid;
	queue_init(&info->queue);
	fd_add(info);
	slog("usbhostd: open fd=%d pid=%d\n", fd, from_pid);
	return 0;
}

static int usb_close(vdevice_t* dev, int fd, int from_pid, uint32_t node, fsinfo_t* fsinfo, void* p) {
	(void)dev;
	(void)node;
	(void)fsinfo;
	(void)p;
	slog("usbhostd: close fd=%d pid=%d\n", fd, from_pid);
	fd_del(fd, from_pid);
	return 0;
}

static int usb_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
		void* buf, int size, int offset, void* p) {
	fd_info_t* info;
	(void)dev;
	(void)node;
	(void)offset;
	(void)p;
	info = fd_find(fd, from_pid);
	if (info == NULL) {
		return VFS_ERR_RETRY;
	}
	return queue_pop(&info->queue, buf, size);
}

static int usb_fcntl(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info,
		int cmd, proto_t* in, proto_t* out, void* p) {
	fd_info_t* item;
	(void)dev;
	(void)info;
	(void)out;
	(void)p;
	item = fd_find(fd, from_pid);
	if (item == NULL) {
		return -1;
	}
	if (cmd == 0) {
		item->report_id = (uint8_t)proto_read_int(in);
		queue_clear(&item->queue);
		slog("usbhostd: subscribe fd=%d pid=%d report_id=%u type=%s\n",
				fd, from_pid, item->report_id,
				item->report_id == USB_REPORT_ID_MOUSE ? usb_input_type_name(USB_INPUT_MOUSE) :
				(item->report_id == USB_REPORT_ID_KEYBOARD ? usb_input_type_name(USB_INPUT_KEYBOARD) :
				(item->report_id == USB_REPORT_ID_TOUCH ? usb_input_type_name(USB_INPUT_TOUCH) :
				usb_input_type_name(USB_INPUT_NONE))));
		return 0;
	}
	return -1;
}

static uint32_t usb_check_poll_events(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node, void* p) {
	fd_info_t* info;
	(void)dev;
	(void)node;
	(void)p;

	info = fd_find(fd, from_pid);
	if (info != NULL && queue_has_data(&info->queue)) {
		return VFS_EVT_RD;
	}
	return 0;
}

int main(int argc, char** argv) {
	const char* mnt_point = argc > 1 ? argv[1] : "/dev/hid0";
	vdevice_t dev;

	slog("usbhostd: start mnt=%s\n", mnt_point);
	_mmio_base = mmio_map();
	if (_mmio_base == 0) {
		slog("usbhostd: mmio_map_failed\n");
		return -1;
	}
	if (dma_pool_init() != 0) {
		slog("usbhostd: dma_init_failed\n");
		return -1;
	}
	if (dwc_host_init() != 0) {
		slog("usbhostd: host_init_failed\n");
		return -1;
	}
	usb_scan_root();
	_next_scan_ms = kernel_tic_ms(0) + USB_SCAN_INTERVAL_MS;

	memset(&dev, 0, sizeof(dev));
	strcpy(dev.name, "usb-hid");
	dev.loop_step = usb_step;
	dev.open = usb_open;
	dev.close = usb_close;
	dev.read = usb_read;
	dev.fcntl = usb_fcntl;
	dev.check_poll_events = usb_check_poll_events;
	slog("usbhostd: device_run name=%s mnt=%s\n", dev.name, mnt_point);
	return device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444);
}
