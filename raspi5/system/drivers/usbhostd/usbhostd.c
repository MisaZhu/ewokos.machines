/*
 * usbhostd for Raspberry Pi 5: USB HID host daemon on /dev/hid0.
 *
 * The Pi4-era DWC2 OTG driver is gone. Pi 5 routes its type-A ports
 * through two DWC3/xHCI controllers in the RP1 southbridge; the register
 * level lives in xhci.c and this file keeps the hardware-independent
 * parts: device enumeration policy, HID report-descriptor parsing,
 * report-ID based fan-out to hid_keybd/hid_moused/hid_touchd, and the
 * vdevice glue.
 *
 * Differences from the old driver that follow from xHCI:
 *  - no manual SET_ADDRESS / data-toggle tracking: Address Device and
 *    per-endpoint state live in the controller;
 *  - interrupt-IN endpoints are scheduled by hardware at their real
 *    bInterval, so the NAK back-off machinery is gone - polling an idle
 *    endpoint is just an event-ring check;
 *  - hubs work through normal class requests, with the TT fields in the
 *    slot context handling LS/FS devices behind a high-speed hub.
 */
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/vfs.h>
#include <ewoksys/mmio.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/klog.h>
#include <ewoksys/syscall.h>
#include <ewoksys/proc.h>
#include <sysinfo.h>
#include <arch/bcm2712/mmio.h>
#include "xhci.h"

/* rp1.dtsi: usb@200000 and usb@300000, xHCI caps at the window base */
#define RP1_XHCI0_OFF (PI5_RP1_WIN_OFF + 0x200000)
#define RP1_XHCI1_OFF (PI5_RP1_WIN_OFF + 0x300000)
#define XHCI_NUM_HCS 2

#define USB_REPORT_ID_MOUSE 1u
#define USB_REPORT_ID_KEYBOARD 2u
#define USB_REPORT_ID_TOUCH 3u

#define USB_QUEUE_DEPTH 32
#define USB_EVENT_SIZE 7
#define USB_MAX_INPUTS 8
#define USB_MAX_DEVS 8
#define USB_MAX_REPORT 64
#define USB_MAX_CANDIDATES 8
#define USB_MAX_USAGE_LIST 32
#define USB_SCAN_INTERVAL_MS 1000u
#define USB_IDLE_SLEEP_MIN_US 1000u
#define USB_IDLE_SLEEP_MAX_US 8000u
#define USB_NO_INPUT_SLEEP_US 20000u
#define USB_MAX_HUB_DEPTH 2

/* enumeration / bring-up logging; per-report traffic stays silent */
#define USB_LOG_ENABLE 1
#if USB_LOG_ENABLE
#define slog(...) klog(__VA_ARGS__)
#else
static inline void usb_log_none(const char* fmt, ...) { (void)fmt; }
#define slog(...) usb_log_none(__VA_ARGS__)
#endif

#define USB_REQ_GET_STATUS 0x00
#define USB_REQ_CLEAR_FEATURE 0x01
#define USB_REQ_SET_FEATURE 0x03
#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_SET_IDLE 0x0A
#define USB_REQ_SET_PROTOCOL 0x0B

#define USB_REQTYPE_STD_IN 0x80
#define USB_REQTYPE_STD_OUT 0x00
#define USB_REQTYPE_STD_IFACE_IN 0x81
#define USB_REQTYPE_STD_EP_OUT 0x02
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
#define USB_CLASS_HID 0x03
#define USB_SUBCLASS_BOOT 0x01
#define USB_PROTOCOL_KEYBOARD 0x01
#define USB_PROTOCOL_MOUSE 0x02

#define USB_FEAT_ENDPOINT_HALT 0

/* hub port features (USB 2.0 spec table 11-17) */
#define USB_HUB_FEAT_PORT_RESET 4
#define USB_HUB_FEAT_PORT_POWER 8
#define USB_HUB_FEAT_C_PORT_CONNECTION 16
#define USB_HUB_FEAT_C_PORT_RESET 20

/* hub wPortStatus bits */
#define USB_HUB_PS_CONNECTION (1u << 0)
#define USB_HUB_PS_ENABLE (1u << 1)
#define USB_HUB_PS_RESET (1u << 4)
#define USB_HUB_PS_LOW_SPEED (1u << 9)
#define USB_HUB_PS_HIGH_SPEED (1u << 10)

/* hub wPortChange bits */
#define USB_HUB_PC_CONNECTION (1u << 0)

#define USB_ENDPOINT_IN 0x80
#define USB_ENDPOINT_XFER_INTERRUPT 0x03

#define HID_USAGE_PAGE_GENERIC_DESKTOP 0x01
#define HID_USAGE_PAGE_DIGITIZER 0x0D
#define HID_USAGE_POINTER 0x01
#define HID_USAGE_MOUSE 0x02
#define HID_USAGE_KEYBOARD 0x06
#define HID_USAGE_TOUCH_SCREEN 0x04
#define HID_USAGE_TOUCH_PAD 0x05
#define HID_USAGE_FINGER 0x22
#define HID_USAGE_TIP_SWITCH 0x42
#define HID_USAGE_X 0x30
#define HID_USAGE_Y 0x31

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

/* ---------------- /dev/hid0 subscriber fan-out ---------------- */

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
	uint8_t interval; /* raw bInterval: the controller schedules it */
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

/* an enumerated USB device (input device or hub) */
typedef struct {
	bool present;
	bool is_hub;
	bool unsupported;    /* enumerated, but no interface we can drive */
	uint8_t hub_ports;
	int8_t parent;       /* _devs index of parent hub, -1 = root port */
	uint8_t parent_port; /* hub port (1-based) when parent >= 0 */
	uint8_t depth;
	xhci_dev_t xdev;
} usb_dev_t;

typedef struct {
	bool present;
	usb_input_type_t type;
	int8_t dev_idx; /* _devs index */
	uint8_t iface_num;
	uint8_t ep_addr;
	uint16_t max_packet;
	uint8_t report_len;
	uint8_t kbd_report_id;   /* composite only */
	uint8_t mouse_report_id; /* composite only */
	touch_parser_t touch;
	uint8_t last_report[USB_MAX_REPORT];
	uint8_t last_len;
} usb_input_dev_t;

static xhci_hc_t _hcs[XHCI_NUM_HCS];
static usb_dev_t _devs[USB_MAX_DEVS];
static usb_input_dev_t _inputs[USB_MAX_INPUTS];
static fd_info_t* _fds = NULL;
static uint64_t _next_scan_ms = 0;
static uint32_t _idle_sleep_us = USB_IDLE_SLEEP_MIN_US;

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

static inline uint16_t le16(const void* p) {
	const uint8_t* b = (const uint8_t*)p;
	return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
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
	while (cur != NULL) {
		if (cur->report_id == report_id) {
			queue_push(&cur->queue, data, len);
		}
		cur = cur->next;
	}
}

/* ---------------- HID report descriptor parsing ---------------- */

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
	bool found_keyboard = false;
	bool found_mouse = false;
	bool found_touch = false;

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
						found_keyboard = true;
					}
					else if (usage == HID_USAGE_MOUSE || usage == HID_USAGE_POINTER) {
						found_mouse = true;
					}
				}
				else if (collection_type == 1 && usage_page == HID_USAGE_PAGE_DIGITIZER) {
					uint32_t usage = hid_usage_for_index(usages, usage_count,
							usage_range_valid, usage_min, usage_max, 0);
					if (usage == HID_USAGE_TOUCH_SCREEN || usage == HID_USAGE_TOUCH_PAD ||
							usage == HID_USAGE_FINGER) {
						found_touch = true;
					}
				}
			}
			usage_count = 0;
			usage_range_valid = false;
		}
	}
	/*
	 * Some USB touch panels expose both Generic Desktop mouse/pointer and
	 * Digitizer touch collections in one report descriptor. Prefer the
	 * explicit digitizer collection, otherwise hid_touchd never gets data.
	 */
	if (found_touch) {
		return HID_DEV_TYPE_TOUCH;
	}
	if (found_keyboard) {
		return HID_DEV_TYPE_KEYBOARD;
	}
	if (found_mouse) {
		return HID_DEV_TYPE_MOUSE;
	}
	return HID_DEV_TYPE_UNKNOWN;
}

/* Detect a composite interface: kbd+mouse collections share one interrupt
   endpoint, distinguished by report IDs. Returns 0 with both IDs filled
   when the descriptor holds a keyboard AND a mouse/pointer application
   collection each with its own report ID. */
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
				if (usage_page == HID_USAGE_PAGE_DIGITIZER &&
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

static bool hid_probe_touch_report(const uint8_t* desc, int len, touch_parser_t* out) {
	touch_parser_t parser;

	if (hid_parse_touch_report(desc, len, &parser) != 0) {
		return false;
	}
	if (out != NULL) {
		*out = parser;
	}
	return true;
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

/* ---------------- control request helpers on top of xhci ---------------- */

static int usb_get_descriptor(xhci_dev_t* xdev, uint8_t req_type,
		uint8_t desc_type, uint8_t index, uint16_t lang_or_iface,
		void* buf, uint16_t size) {
	usb_setup_pkt_t setup;
	memset(&setup, 0, sizeof(setup));
	setup.bmRequestType = req_type;
	setup.bRequest = USB_REQ_GET_DESCRIPTOR;
	setup.wValue = (uint16_t)((desc_type << 8) | index);
	setup.wIndex = lang_or_iface;
	setup.wLength = size;
	return xhci_control_xfer(xdev, &setup, buf, true);
}

static int usb_set_configuration(xhci_dev_t* xdev, uint8_t config) {
	usb_setup_pkt_t setup;
	memset(&setup, 0, sizeof(setup));
	setup.bmRequestType = USB_REQTYPE_STD_OUT;
	setup.bRequest = USB_REQ_SET_CONFIGURATION;
	setup.wValue = config;
	return xhci_control_xfer(xdev, &setup, NULL, false);
}

static int usb_hid_set_protocol(xhci_dev_t* xdev, uint8_t iface, uint8_t protocol) {
	usb_setup_pkt_t setup;
	memset(&setup, 0, sizeof(setup));
	setup.bmRequestType = USB_REQTYPE_CLASS_IFACE_OUT;
	setup.bRequest = USB_REQ_SET_PROTOCOL;
	setup.wValue = protocol; /* 0 = boot, 1 = report */
	setup.wIndex = iface;
	return xhci_control_xfer(xdev, &setup, NULL, false);
}

static int usb_hid_set_idle(xhci_dev_t* xdev, uint8_t iface) {
	usb_setup_pkt_t setup;
	memset(&setup, 0, sizeof(setup));
	setup.bmRequestType = USB_REQTYPE_CLASS_IFACE_OUT;
	setup.bRequest = USB_REQ_SET_IDLE;
	setup.wValue = 0; /* duration 0: report only on change */
	setup.wIndex = iface;
	return xhci_control_xfer(xdev, &setup, NULL, false);
}

static int usb_hub_port_status(xhci_dev_t* xdev, uint8_t port,
		uint16_t* status, uint16_t* change) {
	usb_setup_pkt_t setup;
	uint8_t buf[4];
	memset(&setup, 0, sizeof(setup));
	setup.bmRequestType = USB_REQTYPE_CLASS_PORT_IN;
	setup.bRequest = USB_REQ_GET_STATUS;
	setup.wIndex = port;
	setup.wLength = 4;
	if (xhci_control_xfer(xdev, &setup, buf, true) < 4) {
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

static int usb_hub_port_feature(xhci_dev_t* xdev, uint8_t port,
		uint16_t feature, bool set) {
	usb_setup_pkt_t setup;
	memset(&setup, 0, sizeof(setup));
	setup.bmRequestType = USB_REQTYPE_CLASS_PORT_OUT;
	setup.bRequest = set ? USB_REQ_SET_FEATURE : USB_REQ_CLEAR_FEATURE;
	setup.wValue = feature;
	setup.wIndex = port;
	return xhci_control_xfer(xdev, &setup, NULL, false);
}

/*
 * A STALL leaves the halt latched on both sides: xhci_int_in_poll() already
 * did Reset Endpoint + Set TR Dequeue on the controller side, but the device
 * keeps returning STALL until its own halt feature is cleared (USB 2.0
 * 9.4.5). Without this the endpoint stalls forever after one protocol error
 * and the input goes dead until replug.
 */
static int usb_clear_ep_halt(xhci_dev_t* xdev, uint8_t ep_addr) {
	usb_setup_pkt_t setup;
	memset(&setup, 0, sizeof(setup));
	setup.bmRequestType = USB_REQTYPE_STD_EP_OUT;
	setup.bRequest = USB_REQ_CLEAR_FEATURE;
	setup.wValue = USB_FEAT_ENDPOINT_HALT;
	setup.wIndex = ep_addr;
	return xhci_control_xfer(xdev, &setup, NULL, false);
}

/* ---------------- device table ---------------- */

static void usb_dev_remove(int idx);

static int usb_dev_alloc(void) {
	for (int i = 0; i < USB_MAX_DEVS; ++i) {
		if (!_devs[i].present) {
			return i;
		}
	}
	/*
	 * Parked unsupported devices (see usb_enumerate_device) hold their slot
	 * on purpose, so reclaim one before giving up: a device we can actually
	 * drive is always worth more than a stick we only keep to avoid
	 * re-enumerating it.
	 */
	for (int i = 0; i < USB_MAX_DEVS; ++i) {
		if (_devs[i].unsupported) {
			slog("usbhostd: evict parked dev=%d to free a slot\n", i);
			usb_dev_remove(i);
			return i;
		}
	}
	return -1;
}

static int usb_input_alloc(void) {
	for (int i = 0; i < USB_MAX_INPUTS; ++i) {
		if (!_inputs[i].present) {
			return i;
		}
	}
	return -1;
}

/* drop one device and every input riding on it */
static void usb_dev_remove(int idx) {
	if (idx < 0 || !_devs[idx].present) {
		return;
	}
	for (int i = 0; i < USB_MAX_INPUTS; ++i) {
		if (_inputs[i].present && _inputs[i].dev_idx == idx) {
			slog("usbhostd: remove %s input slot=%d\n",
					usb_input_type_name(_inputs[i].type), i);
			memset(&_inputs[i], 0, sizeof(_inputs[i]));
		}
	}
	xhci_device_detach(&_devs[idx].xdev);
	memset(&_devs[idx], 0, sizeof(_devs[idx]));
}

/* remove a whole subtree: the device plus everything behind it (hubs) */
static void usb_dev_remove_tree(int idx) {
	for (int i = 0; i < USB_MAX_DEVS; ++i) {
		if (_devs[i].present && _devs[i].parent == idx) {
			usb_dev_remove_tree(i);
		}
	}
	usb_dev_remove(idx);
}

static void usb_hc_remove_root(xhci_hc_t* hc, int root_port) {
	for (int i = 0; i < USB_MAX_DEVS; ++i) {
		if (_devs[i].present && _devs[i].parent < 0 &&
				_devs[i].xdev.hc == hc && _devs[i].xdev.root_port == root_port) {
			usb_dev_remove_tree(i);
		}
	}
}

/* ---------------- HID candidate parsing/registration ---------------- */

static int usb_parse_candidates(const uint8_t* cfg, int cfg_len,
		hid_candidate_t* candidates, int max_candidates) {
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

/* open the interrupt endpoint and fill the common input slot fields */
static int usb_input_setup(int dev_idx, const hid_candidate_t* cand,
		usb_input_type_t type) {
	xhci_dev_t* xdev = &_devs[dev_idx].xdev;
	uint16_t mps = cand->max_packet == 0 ? 8 : cand->max_packet;
	int slot;

	if (mps > USB_MAX_REPORT) {
		mps = USB_MAX_REPORT;
	}
	slot = usb_input_alloc();
	if (slot < 0) {
		slog("usbhostd: register %s failed no_slot iface=%u\n",
				usb_input_type_name(type), cand->iface_num);
		return -1;
	}
	if (xhci_int_in_open(xdev, cand->ep_addr, mps, cand->interval) != 0) {
		slog("usbhostd: register %s failed ep_open iface=%u ep=%02x\n",
				usb_input_type_name(type), cand->iface_num, cand->ep_addr);
		return -1;
	}
	memset(&_inputs[slot], 0, sizeof(_inputs[slot]));
	_inputs[slot].present = true;
	_inputs[slot].type = type;
	_inputs[slot].dev_idx = (int8_t)dev_idx;
	_inputs[slot].iface_num = cand->iface_num;
	_inputs[slot].ep_addr = cand->ep_addr;
	_inputs[slot].max_packet = mps;
	return slot;
}

static int usb_register_keyboard(int dev_idx, const hid_candidate_t* cand) {
	xhci_dev_t* xdev = &_devs[dev_idx].xdev;
	int slot;

	/* Set_Protocol is only defined for boot-subclass interfaces; non-boot
	   interfaces default to Report protocol and must not receive it. */
	if (cand->subclass == USB_SUBCLASS_BOOT) {
		(void)usb_hid_set_protocol(xdev, cand->iface_num, 0);
	}
	(void)usb_hid_set_idle(xdev, cand->iface_num);
	slot = usb_input_setup(dev_idx, cand, USB_INPUT_KEYBOARD);
	if (slot < 0) {
		return -1;
	}
	_inputs[slot].report_len = 8;
	slog("usbhostd: register keyboard slot=%d dev=%d iface=%u ep=%02x interval=%u maxpkt=%u\n",
			slot, dev_idx, cand->iface_num, cand->ep_addr, cand->interval, cand->max_packet);
	return 0;
}

static int usb_register_mouse(int dev_idx, const hid_candidate_t* cand) {
	xhci_dev_t* xdev = &_devs[dev_idx].xdev;
	int slot;

	if (cand->subclass == USB_SUBCLASS_BOOT) {
		(void)usb_hid_set_protocol(xdev, cand->iface_num, 0);
	}
	(void)usb_hid_set_idle(xdev, cand->iface_num);
	slot = usb_input_setup(dev_idx, cand, USB_INPUT_MOUSE);
	if (slot < 0) {
		return -1;
	}
	_inputs[slot].report_len = cand->max_packet > 0 ? (uint8_t)_inputs[slot].max_packet : 4;
	slog("usbhostd: register mouse slot=%d dev=%d iface=%u ep=%02x interval=%u maxpkt=%u\n",
			slot, dev_idx, cand->iface_num, cand->ep_addr, cand->interval, cand->max_packet);
	return 0;
}

static int usb_register_composite(int dev_idx, const hid_candidate_t* cand,
		uint8_t kbd_id, uint8_t mouse_id) {
	xhci_dev_t* xdev = &_devs[dev_idx].xdev;
	int slot;

	/* report IDs only exist in Report protocol; boot protocol would strip
	   the mouse collection entirely */
	if (cand->subclass == USB_SUBCLASS_BOOT) {
		(void)usb_hid_set_protocol(xdev, cand->iface_num, 1);
	}
	(void)usb_hid_set_idle(xdev, cand->iface_num);
	slot = usb_input_setup(dev_idx, cand, USB_INPUT_COMPOSITE);
	if (slot < 0) {
		return -1;
	}
	/* variable-size reports: always request a full packet */
	_inputs[slot].report_len = (uint8_t)_inputs[slot].max_packet;
	_inputs[slot].kbd_report_id = kbd_id;
	_inputs[slot].mouse_report_id = mouse_id;
	slog("usbhostd: register composite slot=%d dev=%d iface=%u ep=%02x kbd_id=%u mouse_id=%u\n",
			slot, dev_idx, cand->iface_num, cand->ep_addr, kbd_id, mouse_id);
	return 0;
}

static int usb_register_touch(int dev_idx, const hid_candidate_t* cand,
		const touch_parser_t* parser) {
	xhci_dev_t* xdev = &_devs[dev_idx].xdev;
	int slot;

	/*
	 * Some USB touch panels advertise boot-mouse compatibility on the
	 * interface descriptor, but their real touch data is only available
	 * in Report protocol. Keep them out of Boot protocol here.
	 */
	if (cand->subclass == USB_SUBCLASS_BOOT) {
		(void)usb_hid_set_protocol(xdev, cand->iface_num, 1);
	}
	(void)usb_hid_set_idle(xdev, cand->iface_num);
	slot = usb_input_setup(dev_idx, cand, USB_INPUT_TOUCH);
	if (slot < 0) {
		return -1;
	}
	_inputs[slot].report_len = parser->report_bytes;
	_inputs[slot].touch = *parser;
	slog("usbhostd: register touch slot=%d dev=%d iface=%u ep=%02x report_id=%u report_len=%u tip=%d x=%d y=%d\n",
			slot, dev_idx, cand->iface_num, cand->ep_addr, parser->report_id,
			parser->report_bytes, parser->tip_bit, parser->x_bit, parser->y_bit);
	return 0;
}

/* ---------------- enumeration ---------------- */

static int usb_enumerate_hub(int dev_idx);

/*
 * Enumerate one freshly reset device. parent < 0 means it sits on a root
 * port of hc; otherwise it hangs off _devs[parent] hub port hub_port.
 * speed is XHCI_SPEED_*. Returns the number of registered inputs (a hub
 * counts its children), or < 0 on failure.
 */
static int usb_enumerate_device(xhci_hc_t* hc, int root_port, int speed,
		int parent, int hub_port) {
	usb_device_desc_t dev_desc;
	uint8_t cfg_head[9];
	uint8_t* cfg_buf = NULL;
	uint16_t total_len;
	int cand_count;
	hid_candidate_t candidates[USB_MAX_CANDIDATES];
	int registered = 0;
	int dev_idx;
	uint8_t depth = parent >= 0 ? _devs[parent].depth + 1 : 0;

	dev_idx = usb_dev_alloc();
	if (dev_idx < 0) {
		slog("usbhostd: enumerate no_dev_slot\n");
		return -1;
	}
	usb_dev_t* dev = &_devs[dev_idx];
	memset(dev, 0, sizeof(*dev));

	if (xhci_device_attach(hc, root_port, speed,
			parent >= 0 ? &_devs[parent].xdev : NULL, hub_port, &dev->xdev) != 0) {
		return -1;
	}
	dev->present = true;
	dev->parent = (int8_t)parent;
	dev->parent_port = (uint8_t)(parent >= 0 ? hub_port : 0);
	dev->depth = depth;

	/* first 8 bytes reveal bMaxPacketSize0 before the full descriptor */
	memset(&dev_desc, 0, sizeof(dev_desc));
	if (usb_get_descriptor(&dev->xdev, USB_REQTYPE_STD_IN,
			USB_DESC_DEVICE, 0, 0, &dev_desc, 8) < 8) {
		slog("usbhostd: enumerate get_desc8_failed\n");
		usb_dev_remove(dev_idx);
		return -1;
	}
	if (xhci_update_mps0(&dev->xdev, dev_desc.bMaxPacketSize0) != 0) {
		slog("usbhostd: enumerate update_mps0_failed mps0=%u\n",
				dev_desc.bMaxPacketSize0);
		usb_dev_remove(dev_idx);
		return -1;
	}
	if (usb_get_descriptor(&dev->xdev, USB_REQTYPE_STD_IN,
			USB_DESC_DEVICE, 0, 0, &dev_desc, sizeof(dev_desc)) < (int)sizeof(dev_desc)) {
		slog("usbhostd: enumerate get_device_desc_failed\n");
		usb_dev_remove(dev_idx);
		return -1;
	}
	slog("usbhostd: dev=%d slot=%u vid=%04x pid=%04x class=%02x mps0=%u speed=%d depth=%u\n",
			dev_idx, dev->xdev.slot_id, dev_desc.idVendor, dev_desc.idProduct,
			dev_desc.bDeviceClass, dev_desc.bMaxPacketSize0, speed, depth);

	if (usb_get_descriptor(&dev->xdev, USB_REQTYPE_STD_IN,
			USB_DESC_CONFIG, 0, 0, cfg_head, sizeof(cfg_head)) < (int)sizeof(cfg_head)) {
		slog("usbhostd: enumerate get_config_head_failed\n");
		usb_dev_remove(dev_idx);
		return -1;
	}
	total_len = le16(cfg_head + 2);
	if (total_len < sizeof(usb_config_desc_t) || total_len > 1024) {
		slog("usbhostd: enumerate invalid_config_len total=%u\n", total_len);
		usb_dev_remove(dev_idx);
		return -1;
	}

	cfg_buf = (uint8_t*)malloc(total_len);
	if (cfg_buf == NULL) {
		usb_dev_remove(dev_idx);
		return -1;
	}
	if (usb_get_descriptor(&dev->xdev, USB_REQTYPE_STD_IN,
			USB_DESC_CONFIG, 0, 0, cfg_buf, total_len) < total_len) {
		slog("usbhostd: enumerate get_config_failed total=%u\n", total_len);
		free(cfg_buf);
		usb_dev_remove(dev_idx);
		return -1;
	}

	if (dev_desc.bDeviceClass == USB_CLASS_HUB) {
		free(cfg_buf);
		if (depth >= USB_MAX_HUB_DEPTH) {
			slog("usbhostd: hub too deep depth=%u\n", depth);
			usb_dev_remove(dev_idx);
			return 0;
		}
		if (usb_set_configuration(&dev->xdev, ((usb_config_desc_t*)cfg_head)->bConfigurationValue) < 0) {
			slog("usbhostd: hub set_config_failed\n");
			usb_dev_remove(dev_idx);
			return -1;
		}
		return usb_enumerate_hub(dev_idx);
	}

	if (usb_set_configuration(&dev->xdev, ((usb_config_desc_t*)cfg_buf)->bConfigurationValue) < 0) {
		slog("usbhostd: enumerate set_config_failed\n");
		free(cfg_buf);
		usb_dev_remove(dev_idx);
		return -1;
	}

	memset(candidates, 0, sizeof(candidates));
	cand_count = usb_parse_candidates(cfg_buf, total_len, candidates, USB_MAX_CANDIDATES);
	free(cfg_buf);
	slog("usbhostd: dev=%d hid_candidates=%d\n", dev_idx, cand_count);

	for (int i = 0; i < cand_count; ++i) {
		uint8_t* report_desc = NULL;
		bool desc_ok = false;
		bool touch_desc_ok = false;
		uint8_t kbd_id = 0, mouse_id = 0;
		hid_dev_type_t dev_type = HID_DEV_TYPE_UNKNOWN;
		touch_parser_t touch_probe;

		if (!candidates[i].valid) {
			continue;
		}
		if (candidates[i].report_desc_len > 0 && candidates[i].report_desc_len <= 1024) {
			report_desc = (uint8_t*)malloc(candidates[i].report_desc_len);
			if (report_desc != NULL &&
					usb_get_descriptor(&dev->xdev, USB_REQTYPE_STD_IFACE_IN,
						USB_DESC_HID_REPORT, 0, candidates[i].iface_num, report_desc,
						candidates[i].report_desc_len) >= candidates[i].report_desc_len) {
				desc_ok = true;
			}
		}
		if (desc_ok) {
			touch_desc_ok = hid_probe_touch_report(report_desc,
					candidates[i].report_desc_len, &touch_probe);
			dev_type = hid_detect_device_type(report_desc, candidates[i].report_desc_len);
			if (touch_desc_ok) {
				dev_type = HID_DEV_TYPE_TOUCH;
			}
		}

		/* composite: boot-keyboard interface whose report descriptor
		   actually multiplexes kbd+mouse via report IDs */
		if (desc_ok && hid_parse_report_ids(report_desc,
				candidates[i].report_desc_len, &kbd_id, &mouse_id) == 0) {
			if (usb_register_composite(dev_idx, &candidates[i], kbd_id, mouse_id) == 0) {
				registered++;
			}
		}
		else if (dev_type == HID_DEV_TYPE_TOUCH && touch_desc_ok) {
			if (usb_register_touch(dev_idx, &candidates[i], &touch_probe) == 0) {
				registered++;
			}
		}
		else if (dev_type == HID_DEV_TYPE_KEYBOARD ||
				(candidates[i].subclass == USB_SUBCLASS_BOOT &&
				 candidates[i].protocol == USB_PROTOCOL_KEYBOARD)) {
			if (usb_register_keyboard(dev_idx, &candidates[i]) == 0) {
				registered++;
			}
		}
		else if (dev_type == HID_DEV_TYPE_MOUSE ||
				(candidates[i].subclass == USB_SUBCLASS_BOOT &&
				 candidates[i].protocol == USB_PROTOCOL_MOUSE)) {
			if (usb_register_mouse(dev_idx, &candidates[i]) == 0) {
				registered++;
			}
		}
		else {
			slog("usbhostd: candidate idx=%d unknown_type skip\n", i);
		}

		if (report_desc != NULL) {
			free(report_desc);
		}
	}

	if (registered == 0) {
		/*
		 * Nothing we can drive on it. Keep the slot: it stays addressed
		 * and configured, so it is quiet on the bus and — more
		 * importantly — usb_scan_hc()/usb_hub_scan() see have_dev and
		 * stop re-running the whole descriptor walk once a second for
		 * every mass-storage stick or printer plugged in. The slot is
		 * released for real when the port reports a disconnect.
		 */
		dev->unsupported = true;
		slog("usbhostd: dev=%d no supported interface, slot parked\n", dev_idx);
		return 0;
	}
	return registered;
}

/* reset one hub port and enumerate whatever shows up behind it */
static int usb_hub_attach_port(int dev_idx, uint8_t port) {
	usb_dev_t* dev = &_devs[dev_idx];
	uint16_t status = 0, change = 0;
	int speed;
	bool enabled = false;

	for (int attempt = 1; attempt <= 3; ++attempt) {
		if (usb_hub_port_feature(&dev->xdev, port, USB_HUB_FEAT_PORT_RESET, true) < 0) {
			slog("usbhostd: hub dev=%d port=%u reset_req_failed\n", dev_idx, port);
			return -1;
		}
		for (int waited = 0; waited < 20; ++waited) {
			proc_usleep(10000);
			if (usb_hub_port_status(&dev->xdev, port, &status, &change) != 0) {
				return -1;
			}
			if ((status & USB_HUB_PS_RESET) == 0 && (status & USB_HUB_PS_ENABLE) != 0) {
				enabled = true;
				break;
			}
		}
		usb_hub_port_feature(&dev->xdev, port, USB_HUB_FEAT_C_PORT_RESET, false);
		if (enabled) {
			break;
		}
		proc_usleep(50000);
	}
	if (!enabled) {
		slog("usbhostd: hub dev=%d port=%u reset_failed status=%04x\n",
				dev_idx, port, status);
		return -1;
	}
	proc_usleep(20000); /* post-reset recovery */

	if (status & USB_HUB_PS_LOW_SPEED) {
		speed = XHCI_SPEED_LOW;
	}
	else if (status & USB_HUB_PS_HIGH_SPEED) {
		speed = XHCI_SPEED_HIGH;
	}
	else {
		speed = XHCI_SPEED_FULL;
	}
	slog("usbhostd: hub dev=%d port=%u connected speed=%d\n", dev_idx, port, speed);
	return usb_enumerate_device(dev->xdev.hc, dev->xdev.root_port, speed,
			dev_idx, port);
}

static int usb_enumerate_hub(int dev_idx) {
	usb_dev_t* dev = &_devs[dev_idx];
	usb_setup_pkt_t setup;
	uint8_t hub_desc[9];
	uint8_t num_ports;
	int registered = 0;

	memset(&setup, 0, sizeof(setup));
	setup.bmRequestType = USB_REQTYPE_CLASS_DEV_IN;
	setup.bRequest = USB_REQ_GET_DESCRIPTOR;
	setup.wValue = (uint16_t)(USB_DESC_HUB << 8);
	setup.wLength = sizeof(hub_desc);
	if (xhci_control_xfer(&dev->xdev, &setup, hub_desc, true) < (int)sizeof(hub_desc)) {
		slog("usbhostd: hub dev=%d get_hub_desc_failed\n", dev_idx);
		usb_dev_remove(dev_idx);
		return -1;
	}
	num_ports = hub_desc[2];
	if (num_ports > 8) {
		num_ports = 8;
	}
	dev->is_hub = true;
	dev->hub_ports = num_ports;
	slog("usbhostd: hub dev=%d ports=%u depth=%u\n", dev_idx, num_ports, dev->depth);

	/* the controller needs to know about the hub before children attach */
	if (xhci_configure_hub(&dev->xdev, num_ports) != 0) {
		slog("usbhostd: hub dev=%d configure_hub_failed\n", dev_idx);
		usb_dev_remove(dev_idx);
		return -1;
	}

	for (uint8_t port = 1; port <= num_ports; ++port) {
		usb_hub_port_feature(&dev->xdev, port, USB_HUB_FEAT_PORT_POWER, true);
	}
	/* bPwrOn2PwrGood is in 2ms units; add margin for slow rails */
	proc_usleep(((uint32_t)hub_desc[5] * 2u + 100u) * 1000u);

	for (uint8_t port = 1; port <= num_ports; ++port) {
		uint16_t status = 0, change = 0;
		int ret;

		if (usb_hub_port_status(&dev->xdev, port, &status, &change) != 0) {
			continue;
		}
		if (change & USB_HUB_PC_CONNECTION) {
			usb_hub_port_feature(&dev->xdev, port, USB_HUB_FEAT_C_PORT_CONNECTION, false);
		}
		if ((status & USB_HUB_PS_CONNECTION) == 0) {
			continue;
		}
		ret = usb_hub_attach_port(dev_idx, port);
		if (ret > 0) {
			registered += ret;
		}
	}
	/* the hub device itself stays registered even with no children yet:
	   the periodic scan keeps watching its ports */
	return registered;
}

/* poll a live hub for connect/disconnect changes on its ports */
static void usb_hub_scan(int dev_idx) {
	usb_dev_t* dev = &_devs[dev_idx];

	for (uint8_t port = 1; port <= dev->hub_ports; ++port) {
		uint16_t status = 0, change = 0;

		if (usb_hub_port_status(&dev->xdev, port, &status, &change) != 0) {
			continue;
		}
		if ((change & USB_HUB_PC_CONNECTION) == 0) {
			continue;
		}
		usb_hub_port_feature(&dev->xdev, port, USB_HUB_FEAT_C_PORT_CONNECTION, false);

		/* drop whatever was on this port */
		for (int i = 0; i < USB_MAX_DEVS; ++i) {
			if (_devs[i].present && _devs[i].parent == dev_idx &&
					_devs[i].parent_port == port) {
				slog("usbhostd: hub dev=%d port=%u disconnected\n", dev_idx, port);
				usb_dev_remove_tree(i);
			}
		}
		if (status & USB_HUB_PS_CONNECTION) {
			usb_hub_attach_port(dev_idx, port);
		}
	}
}

/* check root ports of one controller for connect/disconnect */
static void usb_scan_hc(xhci_hc_t* hc) {
	uint32_t changes;

	if (!hc->present) {
		return;
	}
	xhci_process_events(hc);
	changes = xhci_port_take_changes(hc);

	for (uint32_t p = 1; p <= hc->num_ports; ++p) {
		bool connected = xhci_port_connected(hc, (int)p);
		bool have_dev = false;

		for (int i = 0; i < USB_MAX_DEVS; ++i) {
			if (_devs[i].present && _devs[i].parent < 0 &&
					_devs[i].xdev.hc == hc && _devs[i].xdev.root_port == (int)p) {
				have_dev = true;
				break;
			}
		}

		if ((changes & (1u << (p - 1))) != 0 && have_dev && !connected) {
			slog("usbhostd: xhci%d port=%u disconnected\n", hc->id, p);
			usb_hc_remove_root(hc, (int)p);
			have_dev = false;
		}
		if (connected && !have_dev) {
			int speed = xhci_port_reset(hc, (int)p);
			if (speed < 0) {
				continue;
			}
			slog("usbhostd: xhci%d port=%u connected speed=%d\n", hc->id, p, speed);
			usb_enumerate_device(hc, (int)p, speed, -1, 0);
		}
	}
}

/* ---------------- input polling / dispatch ---------------- */

static bool usb_poll_inputs(vdevice_t* dev) {
	uint8_t report[USB_MAX_REPORT];
	uint8_t payload[USB_EVENT_SIZE];
	bool wakeup = false;

	for (int i = 0; i < USB_MAX_INPUTS; ++i) {
		usb_input_dev_t* in = &_inputs[i];
		int ret;

		if (!in->present) {
			continue;
		}
		memset(report, 0, sizeof(report));
		ret = xhci_int_in_poll(&_devs[in->dev_idx].xdev, in->ep_addr,
				report, sizeof(report));
		if (ret == -2) {
			/* STALL: the controller-side ring is already recovered, now
			   clear the device-side halt or it stalls again forever */
			slog("usbhostd: input slot=%d ep=%02x stalled, clearing halt\n",
					i, in->ep_addr);
			(void)usb_clear_ep_halt(&_devs[in->dev_idx].xdev, in->ep_addr);
			continue;
		}
		if (ret <= 0) {
			/* 0: hardware still polling the device; <0: transient error,
			   the ring was recovered and the next call re-arms it */
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
			wakeup = true;
		}
		else if (in->type == USB_INPUT_MOUSE) {
			memset(payload, 0, sizeof(payload));
			memcpy(payload, report, ret > USB_EVENT_SIZE ? USB_EVENT_SIZE : ret);
			dispatch_data(USB_REPORT_ID_MOUSE, payload, USB_EVENT_SIZE);
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
				wakeup = true;
			}
			else if (rid == in->mouse_report_id) {
				memset(payload, 0, sizeof(payload));
				memcpy(payload, report + 1, (ret - 1) > USB_EVENT_SIZE ? USB_EVENT_SIZE : (ret - 1));
				dispatch_data(USB_REPORT_ID_MOUSE, payload, USB_EVENT_SIZE);
				wakeup = true;
			}
			/* other report IDs (gamepad etc.): no consumer yet, drop */
		}
	}

	if (wakeup) {
		vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
	}
	return wakeup;
}

/* ---------------- vdevice glue ---------------- */

static int usb_step(vdevice_t* dev, void* p) {
	uint64_t now = kernel_tic_ms(0);
	bool have_inputs = false;
	bool got;
	(void)p;

	if (now >= _next_scan_ms) {
		_next_scan_ms = now + USB_SCAN_INTERVAL_MS;
		for (int i = 0; i < XHCI_NUM_HCS; ++i) {
			usb_scan_hc(&_hcs[i]);
		}
		for (int i = 0; i < USB_MAX_DEVS; ++i) {
			if (_devs[i].present && _devs[i].is_hub) {
				usb_hub_scan(i);
			}
		}
	}

	for (int i = 0; i < XHCI_NUM_HCS; ++i) {
		if (_hcs[i].present) {
			xhci_process_events(&_hcs[i]);
		}
	}
	got = usb_poll_inputs(dev);

	for (int i = 0; i < USB_MAX_INPUTS; ++i) {
		if (_inputs[i].present) {
			have_inputs = true;
			break;
		}
	}
	/*
	 * The controller schedules the interrupt endpoints in hardware; the
	 * loop only drains the event ring, so sleeping is cheap. Back off
	 * while idle, snap back to the floor as soon as a report arrives.
	 */
	if (got) {
		_idle_sleep_us = USB_IDLE_SLEEP_MIN_US;
	}
	else if (_idle_sleep_us < USB_IDLE_SLEEP_MAX_US) {
		_idle_sleep_us <<= 1;
		if (_idle_sleep_us > USB_IDLE_SLEEP_MAX_US) {
			_idle_sleep_us = USB_IDLE_SLEEP_MAX_US;
		}
	}
	proc_usleep(have_inputs ? _idle_sleep_us : USB_NO_INPUT_SLEEP_US);
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
	int found = 0;

	/* map the main MMIO window plus the RP1 window, like the other RP1
	   users (uartd, i2c, spi) */
	sys_info_t sysinfo;
	syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&sysinfo);
	_mmio_base = sysinfo.mmio.v_base;
	syscall3(SYS_MEM_MAP,
			(ewokos_addr_t)sysinfo.mmio.v_base,
			(ewokos_addr_t)sysinfo.mmio.phy_base,
			(ewokos_addr_t)sysinfo.mmio.size);
	/*
	 * sys_mem_map() returns 0 and installs *nothing* when the request misses
	 * check_mem_map_arch()'s whitelist, so an unchecked failure here would
	 * turn the first CAPLENGTH read into a data abort that kills the daemon —
	 * and a dead child leaves ipcserv spinning in ipc_ping() forever.
	 */
	bool rp1_mapped = syscall3(SYS_MEM_MAP,
			_mmio_base + PI5_RP1_WIN_OFF,
			PI5_RP1_PHY,
			PI5_RP1_WIN_SIZE) != 0;

	/*
	 * Never bail out before device_run(): ipcserv blocks in ipc_wait_ready()
	 * until the child registers its mount point, so exiting here would hang
	 * init.rd forever instead of just losing USB. Degrade instead — every
	 * xhci path is already gated on hc->present, so with no controller the
	 * loop only sleeps and /dev/hid0 stays readable (empty).
	 */
	if (!rp1_mapped) {
		klog("usbhostd: rp1 window map failed, running without usb\n");
	}
	else if (xhci_dma_init() != 0) {
		klog("usbhostd: dma_init_failed, running without usb\n");
	}
	else {
		if (xhci_init(&_hcs[0], 0, _mmio_base + RP1_XHCI0_OFF) == 0) {
			found++;
		}
		if (xhci_init(&_hcs[1], 1, _mmio_base + RP1_XHCI1_OFF) == 0) {
			found++;
		}
	}
	if (found == 0) {
		klog("usbhostd: no xhci controller found, running without usb\n");
	}

	memset(_devs, 0, sizeof(_devs));
	memset(_inputs, 0, sizeof(_inputs));

	/* first scan right away so boot-attached devices come up fast */
	for (int i = 0; i < XHCI_NUM_HCS; ++i) {
		usb_scan_hc(&_hcs[i]);
	}
	_next_scan_ms = kernel_tic_ms(0) + USB_SCAN_INTERVAL_MS;

	memset(&dev, 0, sizeof(dev));
	strcpy(dev.name, "usb-hid");
	dev.loop_step = usb_step;
	dev.open = usb_open;
	dev.close = usb_close;
	dev.read = usb_read;
	dev.fcntl = usb_fcntl;
	dev.check_poll_events = usb_check_poll_events;
	return device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444);
}
