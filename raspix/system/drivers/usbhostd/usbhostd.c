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
#define USB_LOG_TRANSFER_VERBOSE 0
#define DWC_RX_FIFO_SIZE 20480u
#define DWC_NP_TX_FIFO_SIZE 20480u
#define DWC_P_TX_FIFO_SIZE 20480u

#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_REQ_SET_ADDRESS 0x05
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_SET_IDLE 0x0A
#define USB_REQ_SET_PROTOCOL 0x0B

#define USB_REQTYPE_STD_IN 0x80
#define USB_REQTYPE_STD_OUT 0x00
#define USB_REQTYPE_STD_IFACE_IN 0x81
#define USB_REQTYPE_CLASS_IFACE_OUT 0x21

#define USB_DESC_DEVICE 0x01
#define USB_DESC_CONFIG 0x02
#define USB_DESC_INTERFACE 0x04
#define USB_DESC_ENDPOINT 0x05
#define USB_DESC_HID 0x21
#define USB_DESC_HID_REPORT 0x22

#define USB_CLASS_HID 0x03
#define USB_SUBCLASS_BOOT 0x01
#define USB_PROTOCOL_KEYBOARD 0x01
#define USB_PROTOCOL_MOUSE 0x02

#define USB_ENDPOINT_IN 0x80
#define USB_ENDPOINT_XFER_INTERRUPT 0x03

#define HID_USAGE_PAGE_GENERIC_DESKTOP 0x01
#define HID_USAGE_PAGE_DIGITIZER 0x0D
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
#define DWC_REG_HPTXFSIZ 0x100
#define DWC_REG_HCFG 0x400
#define DWC_REG_HFIR 0x404
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
#define DWC_HCCHAR_PKTCNT_SHIFT 20
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
	touch_parser_t touch;
	uint8_t last_report[USB_MAX_REPORT];
	uint8_t last_len;
	uint64_t next_poll_ms;
	uint64_t last_log_ms;
} usb_input_dev_t;

static dma_pool_t _dma_pool;
static fd_info_t* _fds = NULL;
static usb_input_dev_t _inputs[USB_MAX_INPUTS];
static uint32_t _usb_base = 0;
static bool _device_ready = false;
static bool _port_connected = false;
static uint8_t _next_address = 2;
static uint32_t _scan_ticks = 0;

static const char* usb_input_type_name(usb_input_type_t type) {
	switch (type) {
	case USB_INPUT_KEYBOARD:
		return "keyboard";
	case USB_INPUT_MOUSE:
		return "mouse";
	case USB_INPUT_TOUCH:
		return "touch";
	default:
		return "unknown";
	}
}

static const char* usb_speed_name(bool low_speed) {
	return low_speed ? "low" : "full";
}

static void usb_log_keyboard_event(const usb_input_dev_t* in, const uint8_t* report, int len) {
	if (len < 8) {
		slog("usbhostd: keybd addr=%u iface=%u short_report len=%d\n",
				in->addr, in->iface_num, len);
		return;
	}
	slog("usbhostd: keybd addr=%u iface=%u mod=%02x keys=%02x %02x %02x %02x %02x %02x\n",
			in->addr, in->iface_num, report[0],
			report[2], report[3], report[4], report[5], report[6], report[7]);
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
	slog("usbhostd: mouse addr=%u iface=%u btn=%02x dx=%d dy=%d wheel=%d len=%d\n",
			in->addr, in->iface_num, report[0], dx, dy, wheel, len);
}

static void usb_log_touch_event(const usb_input_dev_t* in, const uint8_t* payload) {
	uint16_t x = (uint16_t)payload[1] | ((uint16_t)payload[2] << 8);
	uint16_t y = (uint16_t)payload[3] | ((uint16_t)payload[4] << 8);
	slog("usbhostd: touch addr=%u iface=%u pressed=%u x=%u y=%u\n",
			in->addr, in->iface_num, payload[0], x, y);
}

static void usb_log_input_event(usb_input_dev_t* in, const uint8_t* report, int len, const uint8_t* payload) {
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
		usb_log_touch_event(in, payload);
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
	if (queue->rd == queue->wr) {
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
		slog("usbhostd: drop report report_id=%u len=%u no_listener\n", report_id, len);
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
		slog("usbhostd: ch%d halt timeout hcchar=%08x hcint=%08x\n",
				ch, usb_readl(DWC_HCCHAR(ch)), usb_readl(DWC_HCINT(ch)));
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
	int ch;

	for (ch = 0; ch < 16; ch++) {
		uint32_t hcchar = DWC_HCCHAR_CHDIS | DWC_HCCHAR_EPDIR_IN;
		usb_writel(DWC_HCINT(ch), 0xFFFFFFFFu);
		usb_writel(DWC_HCINTMSK(ch), 0);
		usb_writel(DWC_HCCHAR(ch), hcchar);
	}

	for (ch = 0; ch < 16; ch++) {
		uint32_t waited = 0;
		uint32_t hcchar = usb_readl(DWC_HCCHAR(ch));
		hcchar |= DWC_HCCHAR_CHDIS | DWC_HCCHAR_CHENA | DWC_HCCHAR_EPDIR_IN;
		usb_writel(DWC_HCCHAR(ch), hcchar);
		while ((usb_readl(DWC_HCCHAR(ch)) & DWC_HCCHAR_CHENA) != 0) {
			if (waited++ > 100) {
				slog("usbhostd: host halt channel_timeout ch=%d hcchar=%08x hcint=%08x\n",
						ch, usb_readl(DWC_HCCHAR(ch)), usb_readl(DWC_HCINT(ch)));
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

	slog("usbhostd: port reset begin\n");
	dwc_port_write(DWC_HPRT_PWR, 0);
	proc_usleep(50000);
	if (!dwc_port_connected()) {
		slog("usbhostd: port reset abort no_device\n");
		return -1;
	}

	dwc_port_write(DWC_HPRT_PWR | DWC_HPRT_RST, 0);
	proc_usleep(60000);
	dwc_port_write(DWC_HPRT_PWR, DWC_HPRT_RST);
	proc_usleep(10000);
	for (;;) {
		reg = usb_readl(DWC_REG_HPRT);
		if ((reg & 0x1u) == 0) {
			slog("usbhostd: port reset failed disconnected\n");
			return -1;
		}
		if ((reg & DWC_HPRT_ENA) != 0) {
			break;
		}
		if (waited_ms++ >= 100u) {
			slog("usbhostd: port reset failed not_enabled hprt=%08x\n", reg);
			return -1;
		}
		proc_usleep(1000);
	}
	dwc_ack_port_change();

	reg = usb_readl(DWC_REG_HPRT);
	if (!dwc_port_connected()) {
		slog("usbhostd: port reset failed disconnected\n");
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

	usb_writel(DWC_REG_PCGCR, 0);
	reg = usb_readl(DWC_REG_GUSBCFG);
	reg &= ~(DWC_GUSBCFG_PHYIF | DWC_GUSBCFG_SRPCAP | DWC_GUSBCFG_HNPCAP |
			DWC_GUSBCFG_ULPI_FSLS | DWC_GUSBCFG_ULPI_CLK_SUSP_M |
			DWC_GUSBCFG_ULPI_DRV_EXT_VBUS | DWC_GUSBCFG_TSDLINEPULSE |
			DWC_GUSBCFG_FORCE_DEV_MODE);
	reg |= DWC_GUSBCFG_FORCE_HOST_MODE | DWC_GUSBCFG_ULPI_UTMI_SEL;
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
	reg &= ~(DWC_GUSBCFG_PHYIF | DWC_GUSBCFG_SRPCAP | DWC_GUSBCFG_HNPCAP |
			DWC_GUSBCFG_ULPI_FSLS | DWC_GUSBCFG_ULPI_CLK_SUSP_M |
			DWC_GUSBCFG_ULPI_DRV_EXT_VBUS | DWC_GUSBCFG_TSDLINEPULSE |
			DWC_GUSBCFG_FORCE_DEV_MODE);
	reg |= DWC_GUSBCFG_FORCE_HOST_MODE | DWC_GUSBCFG_ULPI_UTMI_SEL;
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
	slog("usbhostd: host init ready gahbcfg=%08x gintmsk=%08x hcfg=%08x hfir=%08x hprt=%08x\n",
			usb_readl(DWC_REG_GAHBCFG), usb_readl(DWC_REG_GINTMSK),
			usb_readl(DWC_REG_HCFG), usb_readl(DWC_REG_HFIR), usb_readl(DWC_REG_HPRT));
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
			((uint32_t)(dev_addr & 0x7Fu) << DWC_HCCHAR_DEVADDR_SHIFT);
	if (dir_in) {
		hcchar |= DWC_HCCHAR_EPDIR_IN;
	}
	if (low_speed) {
		hcchar |= DWC_HCCHAR_LSPDDEV;
	}
	dwc_writel_sync(DWC_HCCHAR(ch), hcchar);

	hcchar_start = usb_readl(DWC_HCCHAR(ch));
	hcchar_start &= ~DWC_HCCHAR_CHDIS;
	hcchar_start |= (1u << DWC_HCCHAR_PKTCNT_SHIFT) | DWC_HCCHAR_CHENA;
	if (USB_LOG_TRANSFER_VERBOSE && ep_num == 0) {
		slog("usbhostd: ch%d start addr=%u ep=%u dir=%s type=%u pid=%u mps=%u len=%u dma_phys=%08x dma_bus=%08x hcchar=%08x hctsiz=%08x hcsplt=%08x hprt=%08x\n",
				ch, dev_addr, ep_num, dir_in ? "in" : "out", ep_type, pid,
				max_packet, length, buffer_phys, buffer_bus, hcchar_start, hctsiz,
				usb_readl(DWC_HCSPLT(ch)), usb_readl(DWC_REG_HPRT));
	}
	dwc_writel_sync(DWC_HCCHAR(ch), hcchar_start);

	if (dwc_channel_wait(ch, timeout_ms, &hcint, &remaining) != 0) {
		slog("usbhostd: ch%d timeout addr=%u ep=%u dir=%s len=%u pid=%u hcchar=%08x hcint=%08x hctsiz=%08x hprt=%08x\n",
				ch, dev_addr, ep_num, dir_in ? "in" : "out", length, pid,
				usb_readl(DWC_HCCHAR(ch)), usb_readl(DWC_HCINT(ch)),
				usb_readl(DWC_HCTSIZ(ch)), usb_readl(DWC_REG_HPRT));
		(void)dwc_channel_halt(ch, 20);
		return -1;
	}

	usb_writel(DWC_HCINT(ch), hcint);
	if (hcint & (DWC_HCINT_AHBERR | DWC_HCINT_STALL | DWC_HCINT_TXERR |
			DWC_HCINT_BBLERR | DWC_HCINT_DTERR)) {
		slog("usbhostd: ch%d error addr=%u ep=%u dir=%s hcint=%08x hcchar=%08x hctsiz=%08x len=%u\n",
				ch, dev_addr, ep_num, dir_in ? "in" : "out", hcint,
				usb_readl(DWC_HCCHAR(ch)), usb_readl(DWC_HCTSIZ(ch)), length);
		(void)dwc_channel_halt(ch, 20);
		return -1;
	}
	if (hcint & (DWC_HCINT_NAK | DWC_HCINT_NYET | DWC_HCINT_FRMOVRUN)) {
		(void)dwc_channel_halt(ch, 20);
		return USB_XFER_RETRY;
	}
	if ((hcint & DWC_HCINT_XFRC) == 0 && (hcint & DWC_HCINT_CHH) != 0 && length != 0) {
		slog("usbhostd: ch%d halted addr=%u ep=%u dir=%s hcint=%08x hcchar=%08x hctsiz=%08x len=%u\n",
				ch, dev_addr, ep_num, dir_in ? "in" : "out", hcint,
				usb_readl(DWC_HCCHAR(ch)), usb_readl(DWC_HCTSIZ(ch)), length);
		(void)dwc_channel_halt(ch, 20);
		return -1;
	}
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
		slog("usbhostd: ctrl setup_stage_failed addr=%u req=%02x\n", addr, setup->bRequest);
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
			slog("usbhostd: ctrl data_stage_failed addr=%u req=%02x dir=%s len=%u\n",
					addr, setup->bRequest, data_in ? "in" : "out", setup->wLength);
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
		slog("usbhostd: ctrl status_stage_failed addr=%u req=%02x dir=%s\n",
				addr, setup->bRequest, data_in ? "out" : "in");
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
	if (ret > 0) {
		memcpy(data, payload, ret);
		dev->toggle ^= 1u;
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
				candidates[count].interval = ep->bInterval == 0 ? 10 : ep->bInterval;
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
	(void)usb_hid_set_protocol(addr, low_speed, ctrl_mps, cand->iface_num, 0);
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
	slog("usbhostd: register keyboard slot=%d addr=%u iface=%u ep=%02x interval=%u maxpkt=%u speed=%s\n",
			slot, addr, cand->iface_num, cand->ep_addr, cand->interval,
			cand->max_packet, usb_speed_name(low_speed));
	return 0;
}

static int usb_register_mouse(uint8_t addr, bool low_speed, uint8_t ctrl_mps, const hid_candidate_t* cand) {
	int slot = usb_input_alloc();
	if (slot < 0) {
		slog("usbhostd: register mouse failed no_slot addr=%u iface=%u\n", addr, cand->iface_num);
		return -1;
	}
	(void)usb_hid_set_protocol(addr, low_speed, ctrl_mps, cand->iface_num, 0);
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
	slog("usbhostd: register mouse slot=%d addr=%u iface=%u ep=%02x interval=%u maxpkt=%u speed=%s\n",
			slot, addr, cand->iface_num, cand->ep_addr, cand->interval,
			cand->max_packet, usb_speed_name(low_speed));
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

static int usb_enumerate_root(void) {
	bool low_speed = false;
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

	slog("usbhostd: enumerate root begin\n");
	usb_inputs_clear();
	if (dwc_reset_port(&low_speed) != 0) {
		slog("usbhostd: enumerate root reset_failed\n");
		return -1;
	}

	if (usb_get_descriptor(0, low_speed, 8, USB_REQTYPE_STD_IN,
			USB_DESC_DEVICE, 0, 0, desc8, sizeof(desc8)) < 8) {
		slog("usbhostd: enumerate root get_device_desc8_failed\n");
		return -1;
	}

	addr = _next_address++;
	if (addr == 0) {
		addr = _next_address++;
	}
	if (usb_set_address(low_speed, addr) < 0) {
		slog("usbhostd: enumerate root set_address_failed addr=%u\n", addr);
		return -1;
	}
	proc_usleep(10000);

	ctrl_mps = desc8[7];
	memset(&dev_desc, 0, sizeof(dev_desc));
	if (usb_get_descriptor(addr, low_speed, ctrl_mps, USB_REQTYPE_STD_IN,
			USB_DESC_DEVICE, 0, 0, &dev_desc, sizeof(dev_desc)) < (int)sizeof(dev_desc)) {
		slog("usbhostd: enumerate root get_device_desc_failed addr=%u\n", addr);
		return -1;
	}
	ctrl_mps = dev_desc.bMaxPacketSize0;
	slog("usbhostd: enumerate root device addr=%u vid=%04x pid=%04x class=%02x subclass=%02x proto=%02x mps0=%u speed=%s\n",
			addr, dev_desc.idVendor, dev_desc.idProduct, dev_desc.bDeviceClass,
			dev_desc.bDeviceSubClass, dev_desc.bDeviceProtocol, ctrl_mps,
			usb_speed_name(low_speed));

	if (usb_get_descriptor(addr, low_speed, ctrl_mps, USB_REQTYPE_STD_IN,
			USB_DESC_CONFIG, 0, 0, cfg_head, sizeof(cfg_head)) < (int)sizeof(cfg_head)) {
		slog("usbhostd: enumerate root get_config_head_failed addr=%u\n", addr);
		return -1;
	}

	total_len = le16(cfg_head + 2);
	if (total_len < sizeof(usb_config_desc_t) || total_len > 1024) {
		slog("usbhostd: enumerate root invalid_config_len addr=%u total=%u\n", addr, total_len);
		return -1;
	}

	cfg_buf = (uint8_t*)malloc(total_len);
	if (cfg_buf == NULL) {
		slog("usbhostd: enumerate root alloc_config_failed addr=%u total=%u\n", addr, total_len);
		return -1;
	}
	if (usb_get_descriptor(addr, low_speed, ctrl_mps, USB_REQTYPE_STD_IN,
			USB_DESC_CONFIG, 0, 0, cfg_buf, total_len) < total_len) {
		slog("usbhostd: enumerate root get_config_failed addr=%u total=%u\n", addr, total_len);
		free(cfg_buf);
		return -1;
	}

	if (usb_set_configuration(addr, low_speed, ctrl_mps, ((usb_config_desc_t*)cfg_buf)->bConfigurationValue) < 0) {
		slog("usbhostd: enumerate root set_config_failed addr=%u cfg=%u\n",
				addr, ((usb_config_desc_t*)cfg_buf)->bConfigurationValue);
		free(cfg_buf);
		return -1;
	}
	proc_usleep(10000);

	memset(candidates, 0, sizeof(candidates));
	cand_count = usb_parse_candidates(cfg_buf, total_len, candidates, USB_MAX_CANDIDATES);
	slog("usbhostd: enumerate root candidates=%d config_len=%u\n", cand_count, total_len);
	free(cfg_buf);

	for (int i = 0; i < cand_count; ++i) {
		if (!candidates[i].valid) {
			continue;
		}
		slog("usbhostd: candidate idx=%d iface=%u subclass=%u proto=%u ep=%02x interval=%u maxpkt=%u report_desc=%u\n",
				i, candidates[i].iface_num, candidates[i].subclass, candidates[i].protocol,
				candidates[i].ep_addr, candidates[i].interval, candidates[i].max_packet,
				candidates[i].report_desc_len);
		if (candidates[i].subclass == USB_SUBCLASS_BOOT && candidates[i].protocol == USB_PROTOCOL_KEYBOARD) {
			if (usb_register_keyboard(addr, low_speed, ctrl_mps, &candidates[i]) == 0) {
				registered++;
			}
		}
		else if (candidates[i].subclass == USB_SUBCLASS_BOOT && candidates[i].protocol == USB_PROTOCOL_MOUSE) {
			if (usb_register_mouse(addr, low_speed, ctrl_mps, &candidates[i]) == 0) {
				registered++;
			}
		}
		else {
			if (usb_register_touch(addr, low_speed, ctrl_mps, &candidates[i]) == 0) {
				registered++;
			}
		}
	}

	slog("usbhostd: enumerate root done addr=%u registered=%d\n", addr, registered);
	return registered > 0 ? registered : -1;
}

static void usb_scan_root(void) {
	bool connected = dwc_port_connected();

	if (!connected) {
		if (_device_ready) {
			slog("usbhostd: port disconnected, clear active inputs\n");
			usb_inputs_clear();
			_device_ready = false;
		}
		else if (_port_connected) {
			slog("usbhostd: port disconnected before ready\n");
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
			slog("usbhostd: device ready inputs=%d\n", found);
		}
		else {
			slog("usbhostd: enumerate root pending\n");
		}
	}
	dwc_ack_port_change();
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
		in->next_poll_ms = now + (in->interval == 0 ? 10u : in->interval);

		memset(report, 0, sizeof(report));
		ret = usb_interrupt_in(in, report, in->report_len == 0 ? in->max_packet : in->report_len);
		if (ret < 0) {
			continue;
		}
		if (ret == 0) {
			continue;
		}

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
	}

	if (wakeup) {
		vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
	}
}

static int usb_step(vdevice_t* dev, void* p) {
	(void)p;
	if ((_scan_ticks++ % 500u) == 0u) {
		usb_scan_root();
	}
	if (_device_ready) {
		usb_poll_inputs(dev);
	}
	proc_usleep(2000);
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

	memset(&dev, 0, sizeof(dev));
	strcpy(dev.name, "usb-hid");
	dev.loop_step = usb_step;
	dev.open = usb_open;
	dev.close = usb_close;
	dev.read = usb_read;
	dev.fcntl = usb_fcntl;
	slog("usbhostd: device_run name=%s mnt=%s\n", dev.name, mnt_point);
	return device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444);
}
