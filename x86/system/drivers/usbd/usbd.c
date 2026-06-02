#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ewoksys/vfs.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/proc.h>
#include <ewoksys/dma.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/klog.h>
#include <bsp/x86_pio.h>

#define PCI_CFG_ADDR_PORT 0xCF8
#define PCI_CFG_DATA_PORT 0xCFC

#define PCI_CLASS_SERIAL_BUS 0x0C
#define PCI_SUBCLASS_USB 0x03
#define PCI_PROGIF_UHCI 0x00
#define PCI_CMD_IO_ENABLE 0x0001
#define PCI_CMD_MEM_ENABLE 0x0002
#define PCI_CMD_BUS_MASTER 0x0004

#define UHCI_MAX_CONTROLLERS 4
#define UHCI_PORTS_PER_CTRL 2
#define UHCI_MAX_INPUT_DEVS 8
#define UHCI_DMA_POOL_SIZE 65536
#define UHCI_FRAME_COUNT 1024
#define UHCI_PTR_TERM 0x00000001u
#define UHCI_PTR_QH 0x00000002u
#define UHCI_PTR_DEPTH 0x00000004u

#define UHCI_REG_USBCMD 0x00
#define UHCI_REG_USBSTS 0x02
#define UHCI_REG_USBINTR 0x04
#define UHCI_REG_FRNUM 0x06
#define UHCI_REG_FRBASEADD 0x08
#define UHCI_REG_SOFMOD 0x0C
#define UHCI_REG_PORTSC1 0x10

#define UHCI_CMD_RS 0x0001
#define UHCI_CMD_HCRESET 0x0002
#define UHCI_CMD_CF 0x0040
#define UHCI_CMD_MAXP 0x0080

#define UHCI_PORT_CCS 0x0001
#define UHCI_PORT_CSC 0x0002
#define UHCI_PORT_PE 0x0004
#define UHCI_PORT_PEC 0x0008
#define UHCI_PORT_LSDA 0x0100
#define UHCI_PORT_RESET 0x0200

#define UHCI_TD_STS_ACTIVE 0x00800000u
#define UHCI_TD_STS_STALLED 0x00400000u
#define UHCI_TD_STS_DBUFERR 0x00200000u
#define UHCI_TD_STS_BABBLE 0x00100000u
#define UHCI_TD_STS_NAK 0x00080000u
#define UHCI_TD_STS_TIMEOUT 0x00040000u
#define UHCI_TD_STS_BITSTUFF 0x00020000u
#define UHCI_TD_STS_LS 0x04000000u
#define UHCI_TD_STS_ERRCNT_SHIFT 27
#define UHCI_TD_STS_ERROR_MASK (UHCI_TD_STS_STALLED | UHCI_TD_STS_DBUFERR | \
		UHCI_TD_STS_BABBLE | UHCI_TD_STS_TIMEOUT | UHCI_TD_STS_BITSTUFF)

#define USB_PID_OUT 0xE1
#define USB_PID_IN 0x69
#define USB_PID_SETUP 0x2D

#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_REQ_SET_ADDRESS 0x05
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_SET_IDLE 0x0A
#define USB_REQ_SET_PROTOCOL 0x0B
#define USB_REQ_GET_REPORT 0x01

#define USB_DESC_DEVICE 0x01
#define USB_DESC_CONFIG 0x02
#define USB_DESC_INTERFACE 0x04
#define USB_DESC_ENDPOINT 0x05

#define USB_DIR_IN 0x80
#define USB_REQTYPE_STD_IN 0x80
#define USB_REQTYPE_STD_OUT 0x00
#define USB_REQTYPE_CLASS_IFACE_IN 0xA1
#define USB_REQTYPE_CLASS_IFACE_OUT 0x21

#define USB_CLASS_HID 0x03
#define USB_SUBCLASS_BOOT 0x01
#define USB_PROTOCOL_KEYBOARD 0x01
#define USB_PROTOCOL_MOUSE 0x02

#define USB_REPORT_ID_MOUSE 1
#define USB_REPORT_ID_KEYBOARD 2

#define KEYBOARD_REPORT_SIZE 8
#define MOUSE_REPORT_SIZE 4
#define REPORT_QUEUE_SIZE 64

typedef struct __attribute__((packed)) {
	uint32_t link_ptr;
	uint32_t ctrl_status;
	uint32_t token;
	uint32_t buffer_ptr;
} uhci_td_t;

typedef struct __attribute__((packed)) {
	uint32_t head_ptr;
	uint32_t element_ptr;
} uhci_qh_t;

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
} usb_interface_desc_t;

typedef struct __attribute__((packed)) {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bEndpointAddress;
	uint8_t bmAttributes;
	uint16_t wMaxPacketSize;
	uint8_t bInterval;
} usb_endpoint_desc_t;

typedef struct report_queue {
	uint8_t data[REPORT_QUEUE_SIZE][KEYBOARD_REPORT_SIZE];
	uint8_t len[REPORT_QUEUE_SIZE];
	uint8_t rd;
	uint8_t wr;
} report_queue_t;

typedef struct fd_info {
	int fd;
	int from_pid;
	uint8_t report_id;
	struct fd_info* next;
} fd_info_t;

typedef struct {
	ewokos_addr_t virt;
	uint32_t phys;
	uint32_t size;
	uint32_t used;
} dma_pool_t;

typedef struct {
	bool present;
	uint8_t bus;
	uint8_t dev;
	uint8_t func;
	uint16_t io_base;
	uhci_qh_t* async_qh;
	uint32_t async_qh_phys;
	uint32_t* frame_list;
	uint32_t frame_list_phys;
} uhci_ctrl_t;

typedef struct {
	bool present;
	uint8_t controller;
	uint8_t port;
	bool low_speed;
	uint8_t address;
	uint8_t endpoint;
	uint8_t iface_num;
	uint8_t type;
	uint8_t toggle;
	uint16_t ep_mps;
	uint8_t ctrl_mps;
	uint8_t interval;
	uint8_t last_report[KEYBOARD_REPORT_SIZE];
	uint8_t last_len;
	uint8_t fail_count;
	uint64_t next_poll_ms;
} usb_input_dev_t;

static dma_pool_t _dma_pool;
static uhci_ctrl_t _ctrls[UHCI_MAX_CONTROLLERS];
static usb_input_dev_t _inputs[UHCI_MAX_INPUT_DEVS];
static uint8_t _next_usb_addr = 1;
static uint32_t _scan_ticks = 0;
static fd_info_t* _fds = NULL;
static report_queue_t _kbd_queue;
static report_queue_t _mouse_queue;
static bool _need_wakeup = false;
static void log_td_statuses(const char* tag, uhci_td_t** tds, int td_count) {
	(void)tag;
	(void)tds;
	(void)td_count;
}

static inline uint32_t align_up(uint32_t value, uint32_t align) {
	return (value + align - 1u) & ~(align - 1u);
}

static uint32_t pci_cfg_addr(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
	return 0x80000000u |
			((uint32_t)bus << 16) |
			((uint32_t)dev << 11) |
			((uint32_t)func << 8) |
			(offset & 0xFCu);
}

static uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
	x86_outl(PCI_CFG_ADDR_PORT, pci_cfg_addr(bus, dev, func, offset));
	return x86_inl(PCI_CFG_DATA_PORT);
}

static uint16_t pci_cfg_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
	uint32_t value = pci_cfg_read32(bus, dev, func, offset);
	return (uint16_t)((value >> ((offset & 0x2u) * 8u)) & 0xFFFFu);
}

static void pci_cfg_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint16_t value) {
	uint32_t shift = (offset & 0x2u) * 8u;
	uint32_t reg = pci_cfg_read32(bus, dev, func, offset);
	reg &= ~(0xFFFFu << shift);
	reg |= ((uint32_t)value << shift);
	x86_outl(PCI_CFG_ADDR_PORT, pci_cfg_addr(bus, dev, func, offset));
	x86_outl(PCI_CFG_DATA_PORT, reg);
}

static void queue_init(report_queue_t* q) {
	memset(q, 0, sizeof(report_queue_t));
}

static bool queue_empty(const report_queue_t* q) {
	return q->rd == q->wr;
}

static void queue_push(report_queue_t* q, const uint8_t* data, uint8_t len) {
	if (len > KEYBOARD_REPORT_SIZE) {
		len = KEYBOARD_REPORT_SIZE;
	}
	memcpy(q->data[q->wr], data, len);
	if (len < KEYBOARD_REPORT_SIZE) {
		memset(q->data[q->wr] + len, 0, KEYBOARD_REPORT_SIZE - len);
	}
	q->len[q->wr] = len;
	q->wr = (uint8_t)((q->wr + 1u) % REPORT_QUEUE_SIZE);
	if (q->wr == q->rd) {
		q->rd = (uint8_t)((q->rd + 1u) % REPORT_QUEUE_SIZE);
	}
	_need_wakeup = true;
}

static int queue_pop(report_queue_t* q, void* buf, int size) {
	int len;
	if (queue_empty(q)) {
		return VFS_ERR_RETRY;
	}
	len = q->len[q->rd];
	if (len > size) {
		len = size;
	}
	memcpy(buf, q->data[q->rd], len);
	q->rd = (uint8_t)((q->rd + 1u) % REPORT_QUEUE_SIZE);
	return len;
}

static fd_info_t* fd_find(int fd, int from_pid) {
	fd_info_t* item = _fds;
	while (item != NULL) {
		if (item->fd == fd && item->from_pid == from_pid) {
			return item;
		}
		item = item->next;
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

static int dma_pool_init(void) {
	_dma_pool.size = UHCI_DMA_POOL_SIZE;
	_dma_pool.used = 0;
	_dma_pool.virt = dma_alloc(0, _dma_pool.size);
	if (_dma_pool.virt == 0) {
		return -1;
	}
	_dma_pool.phys = dma_phy_addr(0, _dma_pool.virt);
	memset((void*)_dma_pool.virt, 0, _dma_pool.size);
	return 0;
}

static void* dma_pool_alloc(uint32_t size, uint32_t align, uint32_t* phys) {
	ewokos_addr_t addr;
	uint32_t aligned;
	if (align == 0) {
		align = 1;
	}
	aligned = align_up(_dma_pool.used, align);
	if ((aligned + size) > _dma_pool.size) {
		return NULL;
	}
	addr = _dma_pool.virt + aligned;
	if (phys != NULL) {
		*phys = _dma_pool.phys + aligned;
	}
	memset((void*)addr, 0, size);
	_dma_pool.used = aligned + size;
	return (void*)addr;
}

static uint32_t dma_pool_mark(void) {
	return _dma_pool.used;
}

static void dma_pool_rewind(uint32_t mark) {
	if (mark <= _dma_pool.size) {
		_dma_pool.used = mark;
	}
}

static inline uint16_t uhci_readw(const uhci_ctrl_t* hc, uint16_t reg) {
	return x86_inw((uint16_t)(hc->io_base + reg));
}

static inline void uhci_writew(const uhci_ctrl_t* hc, uint16_t reg, uint16_t value) {
	x86_outw((uint16_t)(hc->io_base + reg), value);
}

static inline void uhci_writel(const uhci_ctrl_t* hc, uint16_t reg, uint32_t value) {
	x86_outl((uint16_t)(hc->io_base + reg), value);
}

static inline void uhci_writeb(const uhci_ctrl_t* hc, uint16_t reg, uint8_t value) {
	x86_outb((uint16_t)(hc->io_base + reg), value);
}

static inline uint16_t uhci_port_reg(uint8_t port) {
	return (uint16_t)(UHCI_REG_PORTSC1 + (uint16_t)(port * 2u));
}

static uint16_t uhci_port_read(const uhci_ctrl_t* hc, uint8_t port) {
	return uhci_readw(hc, uhci_port_reg(port));
}

static void uhci_port_write(const uhci_ctrl_t* hc, uint8_t port, uint16_t value) {
	uhci_writew(hc, uhci_port_reg(port), value);
}

static uint32_t uhci_td_token(uint8_t pid, uint8_t addr, uint8_t ep, uint8_t toggle, uint16_t len) {
	uint32_t max_len = (len == 0) ? 0x7FFu : ((uint32_t)len - 1u);
	return (uint32_t)pid |
			((uint32_t)addr << 8) |
			((uint32_t)ep << 15) |
			((uint32_t)toggle << 19) |
			(max_len << 21);
}

static uint32_t uhci_td_status(bool low_speed) {
	uint32_t status = UHCI_TD_STS_ACTIVE | (3u << UHCI_TD_STS_ERRCNT_SHIFT);
	if (low_speed) {
		status |= UHCI_TD_STS_LS;
	}
	return status;
}

static uhci_td_t* uhci_td_alloc(uint32_t* phys) {
	return (uhci_td_t*)dma_pool_alloc(sizeof(uhci_td_t), 16, phys);
}

static uint32_t uhci_link_ptr(uint32_t phys, bool is_qh, bool depth_first) {
	if (phys == UHCI_PTR_TERM) {
		return UHCI_PTR_TERM;
	}
	if (is_qh) {
		return phys | UHCI_PTR_QH;
	}
	return phys | (depth_first ? UHCI_PTR_DEPTH : 0u);
}

static void uhci_td_init(uhci_td_t* td, uint32_t next_ptr, uint8_t pid, uint8_t addr,
		uint8_t ep, uint8_t toggle, uint16_t len, uint32_t buffer_phys, bool low_speed) {
	td->link_ptr = uhci_link_ptr(next_ptr, false, true);
	td->ctrl_status = uhci_td_status(low_speed);
	td->token = uhci_td_token(pid, addr, ep, toggle, len);
	td->buffer_ptr = buffer_phys;
}

static int uhci_td_actual_len(const uhci_td_t* td) {
	uint32_t actual = td->ctrl_status & 0x7FFu;
	return actual == 0x7FFu ? 0 : ((int)actual + 1);
}

static int uhci_wait_chain(uhci_td_t** tds, int td_count, uint32_t timeout_ms) {
	uint32_t waited = 0;
	while (waited < timeout_ms) {
		bool done = true;
		for (int i = 0; i < td_count; ++i) {
			if ((tds[i]->ctrl_status & UHCI_TD_STS_ACTIVE) != 0) {
				done = false;
				break;
			}
		}
		if (done) {
			return 0;
		}
		proc_usleep(1000);
		waited++;
	}
	return -1;
}

static int uhci_run_chain(uhci_ctrl_t* hc, uhci_td_t** tds, uint32_t first_phys, int td_count,
		uint32_t timeout_ms) {
	if (td_count <= 0) {
		return -1;
	}
	hc->async_qh->head_ptr = UHCI_PTR_TERM;
	hc->async_qh->element_ptr = uhci_link_ptr(first_phys, false, true);
	uhci_writew(hc, UHCI_REG_USBSTS, 0xFFFF);
	if (uhci_wait_chain(tds, td_count, timeout_ms) != 0) {
		log_td_statuses("run timeout", tds, td_count);
		hc->async_qh->element_ptr = UHCI_PTR_TERM;
		return -1;
	}
	hc->async_qh->element_ptr = UHCI_PTR_TERM;
	return 0;
}

static int uhci_control_msg(uhci_ctrl_t* hc, bool low_speed, uint8_t addr, uint8_t ep_mps,
		const usb_setup_pkt_t* setup, void* data, bool data_in) {
	uint32_t mark = dma_pool_mark();
	usb_setup_pkt_t* setup_dma;
	uint32_t setup_phys = 0;
	uint8_t* payload = NULL;
	uint32_t payload_phys = 0;
	uhci_td_t* tds[64];
	uint32_t td_phys[64];
	int td_count = 0;
	uint8_t toggle = 1;
	uint16_t remaining;
	uint8_t* cursor;
	int ret = -1;
	int bytes_done = 0;

	setup_dma = (usb_setup_pkt_t*)dma_pool_alloc(sizeof(usb_setup_pkt_t), 8, &setup_phys);
	if (setup_dma == NULL) {
		return -1;
	}
	memcpy(setup_dma, setup, sizeof(usb_setup_pkt_t));

	if (setup->wLength > 0) {
		payload = (uint8_t*)dma_pool_alloc(setup->wLength, 8, &payload_phys);
		if (payload == NULL) {
			dma_pool_rewind(mark);
			return -1;
		}
		if (!data_in && data != NULL) {
			memcpy(payload, data, setup->wLength);
		}
	}

	tds[td_count] = uhci_td_alloc(&td_phys[td_count]);
	if (tds[td_count] == NULL) {
		dma_pool_rewind(mark);
		return -1;
	}
	td_count++;

	remaining = setup->wLength;
	cursor = payload;
	while (remaining > 0) {
		uint16_t chunk = remaining;
		if (chunk > ep_mps) {
			chunk = ep_mps;
		}
		if (td_count >= (int)(sizeof(tds) / sizeof(tds[0])) - 1) {
			dma_pool_rewind(mark);
			return -1;
		}
		tds[td_count] = uhci_td_alloc(&td_phys[td_count]);
		if (tds[td_count] == NULL) {
			dma_pool_rewind(mark);
			return -1;
		}
		td_count++;
		cursor += chunk;
		remaining = (uint16_t)(remaining - chunk);
		toggle ^= 1u;
	}

	tds[td_count] = uhci_td_alloc(&td_phys[td_count]);
	if (tds[td_count] == NULL) {
		dma_pool_rewind(mark);
		return -1;
	}
	td_count++;

	uhci_td_init(tds[0],
			td_count > 1 ? td_phys[1] : UHCI_PTR_TERM,
			USB_PID_SETUP, addr, 0, 0, sizeof(usb_setup_pkt_t), setup_phys, low_speed);

	remaining = setup->wLength;
	cursor = payload;
	toggle = 1;
	for (int i = 1; i < td_count - 1; ++i) {
		uint16_t chunk = remaining;
		if (chunk > ep_mps) {
			chunk = ep_mps;
		}
		uhci_td_init(tds[i],
				td_phys[i + 1],
				data_in ? USB_PID_IN : USB_PID_OUT,
				addr, 0, toggle, chunk,
				payload_phys + (uint32_t)(cursor - payload), low_speed);
		cursor += chunk;
		remaining = (uint16_t)(remaining - chunk);
		toggle ^= 1u;
	}

	uhci_td_init(tds[td_count - 1],
			UHCI_PTR_TERM,
			data_in ? USB_PID_OUT : USB_PID_IN,
			addr, 0, 1, 0, 0, low_speed);

	if (uhci_run_chain(hc, tds, td_phys[0], td_count, 200) != 0) {
		dma_pool_rewind(mark);
		return -1;
	}

	for (int i = 0; i < td_count; ++i) {
		if ((tds[i]->ctrl_status & UHCI_TD_STS_ERROR_MASK) != 0) {
			log_td_statuses("ctrl error", tds, td_count);
			dma_pool_rewind(mark);
			return -1;
		}
		if ((tds[i]->ctrl_status & UHCI_TD_STS_NAK) != 0) {
			log_td_statuses("ctrl nak", tds, td_count);
			dma_pool_rewind(mark);
			return -1;
		}
	}

	for (int i = 1; i < td_count - 1; ++i) {
		bytes_done += uhci_td_actual_len(tds[i]);
	}

	if (data_in && data != NULL && setup->wLength > 0) {
		if (bytes_done > setup->wLength) {
			bytes_done = setup->wLength;
		}
		memcpy(data, payload, bytes_done);
	}
	ret = bytes_done;
	dma_pool_rewind(mark);
	return ret;
}

static int uhci_interrupt_in(uhci_ctrl_t* hc, bool low_speed, uint8_t addr, uint8_t ep,
		uint16_t mps, uint8_t toggle, void* data, uint16_t size, uint32_t timeout_ms) {
	uint32_t mark = dma_pool_mark();
	uhci_td_t* td;
	uint32_t td_phys = 0;
	uint8_t* payload;
	uint32_t payload_phys = 0;
	uhci_td_t* list[1];
	int ret = -1;

	if (size == 0) {
		return -1;
	}
	payload = (uint8_t*)dma_pool_alloc(size, 8, &payload_phys);
	if (payload == NULL) {
		return -1;
	}
	td = uhci_td_alloc(&td_phys);
	if (td == NULL) {
		dma_pool_rewind(mark);
		return -1;
	}
	uhci_td_init(td, UHCI_PTR_TERM, USB_PID_IN, addr, ep, toggle, size, payload_phys, low_speed);
	list[0] = td;

	if (uhci_run_chain(hc, list, td_phys, 1, timeout_ms) != 0) {
		dma_pool_rewind(mark);
		return -1;
	}

	if ((td->ctrl_status & UHCI_TD_STS_NAK) != 0) {
		ret = 0;
	}
	else if ((td->ctrl_status & UHCI_TD_STS_ERROR_MASK) != 0) {
		ret = -1;
	}
	else {
		ret = uhci_td_actual_len(td);
		if (ret > 0) {
			if ((uint16_t)ret > size) {
				ret = size;
			}
			memcpy(data, payload, ret);
		}
	}

	dma_pool_rewind(mark);
	return ret;
}

static int usb_get_descriptor(uhci_ctrl_t* hc, bool low_speed, uint8_t addr, uint8_t ep_mps,
		uint8_t desc_type, uint8_t desc_index, void* buf, uint16_t len) {
	usb_setup_pkt_t setup;
	memset(&setup, 0, sizeof(setup));
	setup.bmRequestType = USB_REQTYPE_STD_IN;
	setup.bRequest = USB_REQ_GET_DESCRIPTOR;
	setup.wValue = (uint16_t)(((uint16_t)desc_type << 8) | desc_index);
	setup.wLength = len;
	return uhci_control_msg(hc, low_speed, addr, ep_mps, &setup, buf, true);
}

static int usb_set_address(uhci_ctrl_t* hc, bool low_speed, uint8_t new_addr) {
	usb_setup_pkt_t setup;
	memset(&setup, 0, sizeof(setup));
	setup.bmRequestType = USB_REQTYPE_STD_OUT;
	setup.bRequest = USB_REQ_SET_ADDRESS;
	setup.wValue = new_addr;
	return uhci_control_msg(hc, low_speed, 0, 8, &setup, NULL, false);
}

static int usb_set_configuration(uhci_ctrl_t* hc, bool low_speed, uint8_t addr, uint8_t ep_mps,
		uint8_t config_value) {
	usb_setup_pkt_t setup;
	memset(&setup, 0, sizeof(setup));
	setup.bmRequestType = USB_REQTYPE_STD_OUT;
	setup.bRequest = USB_REQ_SET_CONFIGURATION;
	setup.wValue = config_value;
	return uhci_control_msg(hc, low_speed, addr, ep_mps, &setup, NULL, false);
}

static int usb_hid_set_protocol(uhci_ctrl_t* hc, bool low_speed, uint8_t addr, uint8_t ep_mps,
		uint8_t iface_num) {
	usb_setup_pkt_t setup;
	memset(&setup, 0, sizeof(setup));
	setup.bmRequestType = USB_REQTYPE_CLASS_IFACE_OUT;
	setup.bRequest = USB_REQ_SET_PROTOCOL;
	setup.wIndex = iface_num;
	setup.wValue = 0;
	return uhci_control_msg(hc, low_speed, addr, ep_mps, &setup, NULL, false);
}

static int usb_hid_set_idle(uhci_ctrl_t* hc, bool low_speed, uint8_t addr, uint8_t ep_mps,
		uint8_t iface_num) {
	usb_setup_pkt_t setup;
	memset(&setup, 0, sizeof(setup));
	setup.bmRequestType = USB_REQTYPE_CLASS_IFACE_OUT;
	setup.bRequest = USB_REQ_SET_IDLE;
	setup.wIndex = iface_num;
	setup.wValue = 0;
	return uhci_control_msg(hc, low_speed, addr, ep_mps, &setup, NULL, false);
}

static int usb_hid_get_report(uhci_ctrl_t* hc, bool low_speed, uint8_t addr, uint8_t ep_mps,
		uint8_t iface_num, void* buf, uint16_t len) {
	usb_setup_pkt_t setup;
	memset(&setup, 0, sizeof(setup));
	setup.bmRequestType = USB_REQTYPE_CLASS_IFACE_IN;
	setup.bRequest = USB_REQ_GET_REPORT;
	setup.wIndex = iface_num;
	setup.wValue = 0x0100;
	setup.wLength = len;
	return uhci_control_msg(hc, low_speed, addr, ep_mps, &setup, buf, true);
}

static int uhci_reset_port(uhci_ctrl_t* hc, uint8_t port, bool* low_speed) {
	uint16_t reg;
	uhci_port_write(hc, port, UHCI_PORT_RESET);
	proc_usleep(60000);
	uhci_port_write(hc, port, 0);
	proc_usleep(10000);

	for (int retry = 0; retry < 20; ++retry) {
		reg = uhci_port_read(hc, port);
		uhci_port_write(hc, port, reg | UHCI_PORT_CSC | UHCI_PORT_PEC);
		reg = uhci_port_read(hc, port);
		if ((reg & UHCI_PORT_CCS) == 0) {
			proc_usleep(10000);
			continue;
		}
		reg |= UHCI_PORT_PE;
		uhci_port_write(hc, port, reg | UHCI_PORT_CSC | UHCI_PORT_PEC);
		proc_usleep(10000);
		reg = uhci_port_read(hc, port);
		if ((reg & UHCI_PORT_PE) != 0) {
			if (low_speed != NULL) {
				*low_speed = (reg & UHCI_PORT_LSDA) != 0;
			}
			return 0;
		}
	}
	return -1;
}

static int usb_find_input_slot(uint8_t controller, uint8_t port) {
	for (int i = 0; i < UHCI_MAX_INPUT_DEVS; ++i) {
		if (_inputs[i].present &&
				_inputs[i].controller == controller &&
				_inputs[i].port == port) {
			return i;
		}
	}
	return -1;
}

static int usb_alloc_input_slot(void) {
	for (int i = 0; i < UHCI_MAX_INPUT_DEVS; ++i) {
		if (!_inputs[i].present) {
			return i;
		}
	}
	return -1;
}

static int usb_parse_config_for_hid(const uint8_t* cfg, int cfg_len, usb_input_dev_t* out,
		uint8_t controller, uint8_t port, bool low_speed, uint8_t addr) {
	const usb_interface_desc_t* cur_if = NULL;
	for (int off = 0; off + 2 <= cfg_len; ) {
		uint8_t len = cfg[off];
		uint8_t type = cfg[off + 1];
		if (len < 2 || off + len > cfg_len) {
			break;
		}
		if (type == USB_DESC_INTERFACE && len >= sizeof(usb_interface_desc_t)) {
			cur_if = (const usb_interface_desc_t*)(cfg + off);
		}
		else if (type == USB_DESC_ENDPOINT &&
				len >= sizeof(usb_endpoint_desc_t) &&
				cur_if != NULL &&
				cur_if->bInterfaceClass == USB_CLASS_HID) {
			const usb_endpoint_desc_t* ep = (const usb_endpoint_desc_t*)(cfg + off);
			if ((ep->bEndpointAddress & USB_DIR_IN) != 0 &&
					(ep->bmAttributes & 0x3u) == 0x3u &&
					(cur_if->bInterfaceProtocol == USB_PROTOCOL_KEYBOARD ||
					 cur_if->bInterfaceProtocol == USB_PROTOCOL_MOUSE)) {
				memset(out, 0, sizeof(*out));
				out->present = true;
				out->controller = controller;
				out->port = port;
				// QEMU usb-kbd/usb-mouse on UHCI behave reliably as low-speed
				// even when PORTSC LSDA is not consistently reported here.
				out->low_speed = true;
				out->address = addr;
				out->endpoint = (uint8_t)(ep->bEndpointAddress & 0x0Fu);
				out->iface_num = cur_if->bInterfaceNumber;
				out->type = (cur_if->bInterfaceProtocol == USB_PROTOCOL_KEYBOARD) ?
						USB_REPORT_ID_KEYBOARD : USB_REPORT_ID_MOUSE;
				out->toggle = 0;
				out->ep_mps = (uint16_t)(ep->wMaxPacketSize & 0x07FFu);
				out->ctrl_mps = 8;
				out->interval = ep->bInterval;
				if (out->ep_mps == 0) {
					out->ep_mps = 8;
				}
				return 0;
			}
		}
		off += len;
	}
	return -1;
}

static void usb_remove_port(uint8_t controller, uint8_t port) {
	int slot = usb_find_input_slot(controller, port);
	if (slot >= 0) {
		memset(&_inputs[slot], 0, sizeof(_inputs[slot]));
	}
}

static int usb_enumerate_port(uhci_ctrl_t* hc, uint8_t controller, uint8_t port) {
	bool low_speed = false;
	uint8_t desc8[8];
	usb_device_desc_t device_desc;
	uint8_t cfg_head[9];
	uint8_t* cfg_buf = NULL;
	uint16_t total_len;
	uint8_t addr;
	usb_input_dev_t input;
	int slot;

	if (uhci_reset_port(hc, port, &low_speed) != 0) {
		return -1;
	}
	if (usb_get_descriptor(hc, low_speed, 0, 8, USB_DESC_DEVICE, 0, desc8, sizeof(desc8)) < 8) {
		return -1;
	}

	addr = _next_usb_addr++;
	if (addr == 0) {
		addr = _next_usb_addr++;
	}
	if (usb_set_address(hc, low_speed, addr) < 0) {
		return -1;
	}
	proc_usleep(10000);

	memset(&device_desc, 0, sizeof(device_desc));
	if (usb_get_descriptor(hc, low_speed, addr, desc8[7], USB_DESC_DEVICE, 0,
			&device_desc, sizeof(device_desc)) < (int)sizeof(device_desc)) {
		return -1;
	}

	if (usb_get_descriptor(hc, low_speed, addr, device_desc.bMaxPacketSize0,
			USB_DESC_CONFIG, 0, cfg_head, sizeof(cfg_head)) < (int)sizeof(cfg_head)) {
		return -1;
	}
	total_len = (uint16_t)(cfg_head[2] | ((uint16_t)cfg_head[3] << 8));
	if (total_len < sizeof(usb_config_desc_t) || total_len > 512) {
		return -1;
	}

	cfg_buf = (uint8_t*)malloc(total_len);
	if (cfg_buf == NULL) {
		return -1;
	}
	if (usb_get_descriptor(hc, low_speed, addr, device_desc.bMaxPacketSize0,
			USB_DESC_CONFIG, 0, cfg_buf, total_len) < total_len) {
		free(cfg_buf);
		return -1;
	}

	if (usb_parse_config_for_hid(cfg_buf, total_len, &input, controller, port, low_speed, addr) != 0) {
		free(cfg_buf);
		return -1;
	}
	input.ctrl_mps = device_desc.bMaxPacketSize0;

	if (usb_set_configuration(hc, low_speed, addr, device_desc.bMaxPacketSize0,
			((usb_config_desc_t*)cfg_buf)->bConfigurationValue) < 0) {
		free(cfg_buf);
		return -1;
	}
	free(cfg_buf);

	slot = usb_find_input_slot(controller, port);
	if (slot < 0) {
		slot = usb_alloc_input_slot();
	}
	if (slot < 0) {
		return -1;
	}

	_inputs[slot] = input;
	return 0;
}

static void usb_scan_ports(void) {
	for (int i = 0; i < UHCI_MAX_CONTROLLERS; ++i) {
		uhci_ctrl_t* hc = &_ctrls[i];
		if (!hc->present) {
			continue;
		}
		for (uint8_t port = 0; port < UHCI_PORTS_PER_CTRL; ++port) {
			uint16_t reg = uhci_port_read(hc, port);
			bool connected = (reg & UHCI_PORT_CCS) != 0;
			int slot = usb_find_input_slot((uint8_t)i, port);

			if (!connected) {
				if (slot >= 0) {
					usb_remove_port((uint8_t)i, port);
				}
			}
			else if (slot < 0) {
				(void)usb_enumerate_port(hc, (uint8_t)i, port);
			}

			if ((reg & (UHCI_PORT_CSC | UHCI_PORT_PEC)) != 0) {
				uhci_port_write(hc, port, reg | UHCI_PORT_CSC | UHCI_PORT_PEC);
			}
		}
	}
}

static void uhci_recover_port(uhci_ctrl_t* hc, uint8_t port) {
	uint16_t reg = uhci_port_read(hc, port);

	if ((reg & (UHCI_PORT_CSC | UHCI_PORT_PEC)) != 0) {
		uhci_port_write(hc, port, reg | UHCI_PORT_CSC | UHCI_PORT_PEC);
		reg = uhci_port_read(hc, port);
	}
	if ((reg & UHCI_PORT_CCS) != 0 && (reg & UHCI_PORT_PE) == 0) {
		uhci_port_write(hc, port, reg | UHCI_PORT_PE | UHCI_PORT_CSC | UHCI_PORT_PEC);
		proc_usleep(2000);
		reg = uhci_port_read(hc, port);
	}
}

static int usb_poll_keyboard_input(uhci_ctrl_t* hc, usb_input_dev_t* in, uint8_t* report) {
	static const uint8_t toggle_try[2] = {0, 1};
	int ret;

	ret = uhci_interrupt_in(hc, in->low_speed, in->address, in->endpoint,
			in->ep_mps, in->toggle, report, KEYBOARD_REPORT_SIZE, 20);
	if (ret >= 0) {
		in->fail_count = 0;
		if (ret > 0) {
			in->toggle ^= 1u;
		}
		return ret;
	}

	if (in->fail_count < 3) {
		in->fail_count++;
		uhci_recover_port(hc, in->port);
		return -1;
	}

	ret = uhci_interrupt_in(hc, in->low_speed, in->address, in->endpoint,
			in->ep_mps, in->toggle, report, KEYBOARD_REPORT_SIZE, 30);
	if (ret >= 0) {
		in->fail_count = 0;
		if (ret > 0) {
			in->toggle ^= 1u;
		}
		return ret;
	}

	for (int ti = 0; ti < 2; ++ti) {
		uint8_t try_toggle = toggle_try[ti];

		if (try_toggle == in->toggle) {
			continue;
		}

		ret = uhci_interrupt_in(hc, in->low_speed, in->address, in->endpoint,
				in->ep_mps, try_toggle, report, KEYBOARD_REPORT_SIZE, 30);
		if (ret < 0) {
			continue;
		}
		in->fail_count = 0;
		in->toggle = (uint8_t)(try_toggle ^ 1u);
		return ret;
	}
	in->fail_count = 0;
	uhci_recover_port(hc, in->port);
	return -1;
}

static int usb_poll_mouse_input(uhci_ctrl_t* hc, usb_input_dev_t* in, uint8_t* report) {
	static const uint8_t toggle_try[2] = {0, 1};
	int ret;

	ret = uhci_interrupt_in(hc, in->low_speed, in->address, in->endpoint,
			in->ep_mps, in->toggle, report, MOUSE_REPORT_SIZE, 20);
	if (ret >= 0) {
		in->fail_count = 0;
		if (ret > 0) {
			in->toggle ^= 1u;
		}
		return ret;
	}

	if (in->fail_count < 3) {
		in->fail_count++;
		uhci_recover_port(hc, in->port);
		return -1;
	}

	ret = uhci_interrupt_in(hc, in->low_speed, in->address, in->endpoint,
			in->ep_mps, in->toggle, report, MOUSE_REPORT_SIZE, 30);
	if (ret >= 0) {
		in->fail_count = 0;
		if (ret > 0) {
			in->toggle ^= 1u;
		}
		return ret;
	}

	for (int ti = 0; ti < 2; ++ti) {
		uint8_t try_toggle = toggle_try[ti];

		if (try_toggle == in->toggle) {
			continue;
		}

		ret = uhci_interrupt_in(hc, in->low_speed, in->address, in->endpoint,
				in->ep_mps, try_toggle, report, MOUSE_REPORT_SIZE, 30);
		if (ret < 0) {
			continue;
		}
		in->fail_count = 0;
		in->toggle = (uint8_t)(try_toggle ^ 1u);
		return ret;
	}
	in->fail_count = 0;
	uhci_recover_port(hc, in->port);
	return -1;
}

static void usb_poll_inputs(void) {
	uint8_t report[KEYBOARD_REPORT_SIZE];
	uint64_t now = kernel_tic_ms(0);
	for (int i = 0; i < UHCI_MAX_INPUT_DEVS; ++i) {
		usb_input_dev_t* in = &_inputs[i];
		uhci_ctrl_t* hc;
		int ret;
		uint8_t enqueue[MOUSE_REPORT_SIZE] = {0};
		uint64_t poll_ms;

		if (!in->present) {
			continue;
		}
		poll_ms = in->interval == 0 ? 10u : in->interval;
		if (now < in->next_poll_ms) {
			continue;
		}
		in->next_poll_ms = now + poll_ms;
		hc = &_ctrls[in->controller];
		if (in->type == USB_REPORT_ID_KEYBOARD) {
			ret = usb_poll_keyboard_input(hc, in, report);
		}
		else {
			ret = usb_poll_mouse_input(hc, in, report);
		}
		if (ret < 0) {
			continue;
		}
		if (ret == 0) {
			continue;
		}

		if (in->type == USB_REPORT_ID_KEYBOARD) {
			if (ret > KEYBOARD_REPORT_SIZE) {
				ret = KEYBOARD_REPORT_SIZE;
			}
			if (ret == in->last_len && memcmp(in->last_report, report, ret) == 0) {
				continue;
			}
			memcpy(in->last_report, report, ret);
			in->last_len = ret;
			queue_push(&_kbd_queue, report, (uint8_t)ret);
		}
		else if (in->type == USB_REPORT_ID_MOUSE) {
			if (ret > MOUSE_REPORT_SIZE) {
				ret = MOUSE_REPORT_SIZE;
			}
			memcpy(enqueue, report, ret);
			if (ret < MOUSE_REPORT_SIZE) {
				memset(enqueue + ret, 0, MOUSE_REPORT_SIZE - ret);
			}
			queue_push(&_mouse_queue, enqueue, MOUSE_REPORT_SIZE);
		}
	}
}

static int uhci_init_controller(uhci_ctrl_t* hc) {
	if (hc->frame_list == NULL) {
		hc->frame_list = (uint32_t*)dma_pool_alloc(UHCI_FRAME_COUNT * sizeof(uint32_t),
				4096, &hc->frame_list_phys);
		if (hc->frame_list == NULL) {
			return -1;
		}
	}
	if (hc->async_qh == NULL) {
		hc->async_qh = (uhci_qh_t*)dma_pool_alloc(sizeof(uhci_qh_t), 16, &hc->async_qh_phys);
		if (hc->async_qh == NULL) {
			return -1;
		}
	}

	for (int i = 0; i < UHCI_FRAME_COUNT; ++i) {
		hc->frame_list[i] = hc->async_qh_phys | UHCI_PTR_QH;
	}
	hc->async_qh->head_ptr = UHCI_PTR_TERM;
	hc->async_qh->element_ptr = UHCI_PTR_TERM;

	uhci_writew(hc, UHCI_REG_USBCMD, 0);
	proc_usleep(10000);
	uhci_writew(hc, UHCI_REG_USBCMD, UHCI_CMD_HCRESET);
	for (int i = 0; i < 50; ++i) {
		if ((uhci_readw(hc, UHCI_REG_USBCMD) & UHCI_CMD_HCRESET) == 0) {
			break;
		}
		proc_usleep(1000);
	}

	uhci_writew(hc, UHCI_REG_USBSTS, 0xFFFF);
	uhci_writew(hc, UHCI_REG_USBINTR, 0);
	uhci_writew(hc, UHCI_REG_FRNUM, 0);
	uhci_writel(hc, UHCI_REG_FRBASEADD, hc->frame_list_phys);
	uhci_writeb(hc, UHCI_REG_SOFMOD, 0x40);
	uhci_writew(hc, UHCI_REG_USBCMD, UHCI_CMD_RS | UHCI_CMD_CF | UHCI_CMD_MAXP);
	proc_usleep(10000);
	return 0;
}

static int uhci_probe_all(void) {
	int count = 0;
	for (uint16_t bus = 0; bus < 256 && count < UHCI_MAX_CONTROLLERS; ++bus) {
		for (uint8_t dev = 0; dev < 32 && count < UHCI_MAX_CONTROLLERS; ++dev) {
			for (uint8_t func = 0; func < 8 && count < UHCI_MAX_CONTROLLERS; ++func) {
				uint16_t vendor = pci_cfg_read16((uint8_t)bus, dev, func, 0x00);
				uint32_t class_reg;
				uint8_t class_code;
				uint8_t subclass;
				uint8_t prog_if;
				uint16_t cmd;
				uint16_t io_base = 0;

				if (vendor == 0xFFFF) {
					if (func == 0) {
						break;
					}
					continue;
				}

				class_reg = pci_cfg_read32((uint8_t)bus, dev, func, 0x08);
				class_code = (uint8_t)(class_reg >> 24);
				subclass = (uint8_t)(class_reg >> 16);
				prog_if = (uint8_t)(class_reg >> 8);
				if (class_code != PCI_CLASS_SERIAL_BUS ||
						subclass != PCI_SUBCLASS_USB ||
						prog_if != PCI_PROGIF_UHCI) {
					continue;
				}

				for (uint8_t bar = 0; bar < 6; ++bar) {
					uint32_t barv = pci_cfg_read32((uint8_t)bus, dev, func, (uint8_t)(0x10 + bar * 4));
					if ((barv & 0x1u) != 0 && (barv & ~0x1Fu) != 0) {
						io_base = (uint16_t)(barv & ~0x1Fu);
						break;
					}
				}
				if (io_base == 0) {
					continue;
				}

				cmd = pci_cfg_read16((uint8_t)bus, dev, func, 0x04);
				cmd |= PCI_CMD_IO_ENABLE | PCI_CMD_MEM_ENABLE | PCI_CMD_BUS_MASTER;
				pci_cfg_write16((uint8_t)bus, dev, func, 0x04, cmd);

				memset(&_ctrls[count], 0, sizeof(_ctrls[count]));
				_ctrls[count].present = true;
				_ctrls[count].bus = (uint8_t)bus;
				_ctrls[count].dev = dev;
				_ctrls[count].func = func;
				_ctrls[count].io_base = io_base;
				if (uhci_init_controller(&_ctrls[count]) == 0) {
					count++;
				}
				else {
					memset(&_ctrls[count], 0, sizeof(_ctrls[count]));
				}
			}
		}
	}
	return count;
}

static int usb_open(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info, int oflag, void* p) {
	fd_info_t* item;
	(void)dev;
	(void)info;
	(void)oflag;
	(void)p;
	if (fd < 0) {
		return -1;
	}
	item = (fd_info_t*)calloc(1, sizeof(fd_info_t));
	if (item == NULL) {
		return -1;
	}
	item->fd = fd;
	item->from_pid = from_pid;
	fd_add(item);
	return 0;
}

static int usb_close(vdevice_t* dev, int fd, int from_pid, uint32_t node, fsinfo_t* fsinfo, void* p) {
	(void)dev;
	(void)node;
	(void)fsinfo;
	(void)p;
	fd_del(fd, from_pid);
	return 0;
}

static int usb_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info,
		void* buf, int size, int offset, void* p) {
	fd_info_t* item;
	(void)dev;
	(void)info;
	(void)offset;
	(void)p;
	item = fd_find(fd, from_pid);
	if (item == NULL) {
		return VFS_ERR_RETRY;
	}
	if (item->report_id == USB_REPORT_ID_KEYBOARD) {
		return queue_pop(&_kbd_queue, buf, size);
	}
	if (item->report_id == USB_REPORT_ID_MOUSE) {
		return queue_pop(&_mouse_queue, buf, size);
	}
	return VFS_ERR_RETRY;
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
		return 0;
	}
	return -1;
}

static int usb_loop(vdevice_t* dev, void* p) {
	(void)p;
	usb_poll_inputs();
	if ((_scan_ticks++ % 1000u) == 0) {
		usb_scan_ports();
	}
	if (_need_wakeup) {
		vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
		_need_wakeup = false;
	}
	proc_usleep(2000);
	return 0;
}

int main(int argc, char** argv) {
	const char* mnt_point = argc > 1 ? argv[1] : "/dev/hid0";
	vdevice_t dev;

	if (dma_pool_init() != 0) {
		klog("x86-usb: dma init failed\n");
		return -1;
	}
	queue_init(&_kbd_queue);
	queue_init(&_mouse_queue);

	if (uhci_probe_all() <= 0) {
		klog("x86-usb: no uhci controller found\n");
		return -1;
	}
	usb_scan_ports();

	memset(&dev, 0, sizeof(dev));
	strcpy(dev.name, "usb-hid");
	dev.open = usb_open;
	dev.close = usb_close;
	dev.read = usb_read;
	dev.fcntl = usb_fcntl;
	dev.loop_step = usb_loop;
	return device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444);
}
