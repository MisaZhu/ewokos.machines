#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#include <ewoksys/vdevice.h>
#include <ewoksys/vfs.h>
#include <ewoksys/charbuf.h>
#include <ewoksys/mmio.h>
#include <ewoksys/proc.h>
#include <ewoksys/ipc.h>
#include <ewoksys/dma.h>

#include <arch/bcm283x/gpio.h>
#include <arch/bcm283x/pl011_uart.h>
#include <arch/bcm283x/mailbox.h>

#include "firmware_4345c0.h"

#define MAX_BT_DEVICES 32
#define MAX_HCI_PAYLOAD 260
#define MAX_EVT_LINE 256
#define BT_CMD_RET_SZ 2048
#define BT_UART_PKT_FOLLOW_TIMEOUT_MS 120

#define HCI_PKT_COMMAND 0x01
#define HCI_PKT_ACL 0x02
#define HCI_PKT_EVENT 0x04

#define HCI_OGF_LINK_CTRL 0x01
#define HCI_OGF_HOST_CTRL 0x03
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
#define EVT_PIN_CODE_REQUEST 0x16
#define EVT_LINK_KEY_REQUEST 0x17
#define EVT_LINK_KEY_NOTIFY 0x18
#define EVT_INQUIRY_RESULT_RSSI 0x22
#define EVT_IO_CAPABILITY_REQUEST 0x31
#define EVT_USER_CONFIRMATION_REQUEST 0x33
#define EVT_USER_PASSKEY_REQUEST 0x34
#define EVT_SIMPLE_PAIRING_COMPLETE 0x36
#define EVT_EXTENDED_INQUIRY_RESULT 0x2f

#define BCM2835_MBOX_POWER_DEVID_UART0 1
#define BCM2835_MBOX_TAG_SET_POWER_STATE 0x00028001
#define BCM2835_MBOX_SET_POWER_STATE_REQ_ON (1 << 0)
#define BCM2835_MBOX_SET_POWER_STATE_REQ_WAIT (1 << 1)
#define MAILBOX_VC_ALIAS_NONCACHED 0x40000000u

#define CM_GP2CTL ((uintptr_t)_mmio_base + 0x101080u)
#define CM_GP2DIV ((uintptr_t)_mmio_base + 0x101084u)
#define CM_PASSWORD 0x5a000000u
#define CM_BUSY (1u << 7)
#define CM_ENABLE (1u << 4)

#define UART0_BASE_OFF 0x00201000u
#define UART0_FR_REG ((uintptr_t)_mmio_base + UART0_BASE_OFF + 0x18u)

typedef struct {
	uint32_t buf_size;
	uint32_t code;
} bcm2835_mbox_hdr_t;

typedef struct {
	uint32_t tag;
	uint32_t val_buf_size;
	uint32_t val_len;
} bcm2835_mbox_tag_hdr_t;

typedef struct {
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

typedef struct {
	bcm2835_mbox_hdr_t hdr;
	bcm2835_mbox_tag_set_power_state_t set_power_state;
	uint32_t end_tag;
} msg_set_power_state_t;

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

static vdevice_t* _bt_dev = NULL;
static charbuf_t* _evt_buf = NULL;
static uint32_t _idle_sleep_us = 1000;
static bool _ready = false;
static bool _scanning = false;
static bt_device_t _devices[MAX_BT_DEVICES];
static bt_pending_t _pending;
static bt_wait_cmd_t _wait_cmd;
static bt_wait_debug_t _wait_debug;

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

static uint32_t mailbox_data_from_dma_buf(void* buf) {
	uint32_t phy = dma_phy_addr(0, (ewokos_addr_t)buf);
	if (phy == 0) {
		return 0;
	}
	return (phy + MAILBOX_VC_ALIAS_NONCACHED) >> 4;
}

static int bt_power_on_uart0(void) {
	mail_message_t msg;
	msg_set_power_state_t* req;
	uint32_t mailbox_data;

	req = (msg_set_power_state_t*)dma_alloc(0, sizeof(msg_set_power_state_t));
	if (req == NULL) {
		return -1;
	}

	memset(req, 0, sizeof(*req));
	req->hdr.buf_size = sizeof(*req);
	req->set_power_state.tag_hdr.tag = BCM2835_MBOX_TAG_SET_POWER_STATE;
	req->set_power_state.tag_hdr.val_buf_size = sizeof(req->set_power_state.body);
	req->set_power_state.tag_hdr.val_len = sizeof(req->set_power_state.body.req);
	req->set_power_state.body.req.device_id = BCM2835_MBOX_POWER_DEVID_UART0;
	req->set_power_state.body.req.state =
		BCM2835_MBOX_SET_POWER_STATE_REQ_ON |
		BCM2835_MBOX_SET_POWER_STATE_REQ_WAIT;

	mailbox_data = mailbox_data_from_dma_buf(req);
	if (mailbox_data == 0) {
		dma_free(0, (ewokos_addr_t)req);
		return -1;
	}

	msg.data = mailbox_data;
	msg.channel = PROPERTY_CHANNEL;
	bcm283x_mailbox_call(&msg);
	dma_free(0, (ewokos_addr_t)req);
	return 0;
}

static void bt_enable_gpclk2_32k(void) {
	int timeout = 1000;

	/* CYW43455 combo module needs the 32k reference clock on GPIO43/GPCLK2. */
	bcm283x_gpio_init();
	bcm283x_gpio_config(43, GPIO_ALTF0);
	usleep(20000);

	put32(CM_GP2CTL, CM_PASSWORD | 0x1);
	while (timeout-- > 0) {
		if ((get32(CM_GP2CTL) & CM_BUSY) == 0) {
			break;
		}
		usleep(1000);
	}

	/* 32.768kHz = 19.2MHz / (585 + 3840 / 4096). */
	put32(CM_GP2DIV, CM_PASSWORD | (585u << 12) | 3840u);
	put32(CM_GP2CTL, CM_PASSWORD | (1u << 9) | 0x1);
	put32(CM_GP2CTL, CM_PASSWORD | (1u << 9) | 0x1 | CM_ENABLE);

	timeout = 1000;
	while (timeout-- > 0) {
		if ((get32(CM_GP2CTL) & CM_BUSY) != 0) {
			break;
		}
		usleep(1000);
	}
}

static void bt_prepare_combo_chip_power(void) {
	if (access("/dev/wl0", F_OK) == 0) {
		slog("bluetooth init wl0_present keep_wl_on\n");
		return;
	}

	/* expgpio 1 is WL_ON; keep it enabled when BT is started standalone. */
	slog("bluetooth init standalone enable_wl_on\n");
	bcm283x_mailbox_gpio_config(1, true, true);
	usleep(100000);
}

static void bt_release_bt_shutdown(void) {
	/* expgpio 0 is BT_ON/shutdown-gpios on Raspberry Pi wifi/bt boards. */
	slog("bluetooth init assert_bt_on\n");
	bcm283x_mailbox_gpio_config(0, true, false);
	usleep(100000);
	slog("bluetooth init deassert_bt_on\n");
	bcm283x_mailbox_gpio_config(0, true, true);
	usleep(100000);
}

static int bt_uart_recv_timeout(uint32_t timeout_ms) {
	return bcm283x_pl011_uart_recv(timeout_ms);
}

static int bt_uart_flush(void) {
	int n = 0;

	while (bt_uart_recv_timeout(1) >= 0) {
		++n;
	}
	return n;
}

static void bt_hci_send_packet(uint8_t pkt_type, const uint8_t* data, size_t len) {
	size_t i;

	bcm283x_pl011_uart_send(pkt_type);
	for (i = 0; i < len; ++i) {
		bcm283x_pl011_uart_send(data[i]);
	}
}

static int bt_hci_send_command_raw(uint16_t opcode, const uint8_t* params, uint8_t param_len) {
	uint8_t hdr[3];
	uint8_t i;

	hdr[0] = hci_opcode_lo(opcode);
	hdr[1] = hci_opcode_hi(opcode);
	hdr[2] = param_len;
	bt_hci_send_packet(HCI_PKT_COMMAND, hdr, sizeof(hdr));
	if (param_len > 0 && params != NULL) {
		for (i = 0; i < param_len; ++i) {
			bcm283x_pl011_uart_send(params[i]);
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

static int bt_hci_reject_connection(const uint8_t* addr) {
	uint8_t params[7];

	memcpy(params, addr, 6);
	params[6] = 0x0f;
	return bt_hci_send_command(HCI_OGF_LINK_CTRL, HCI_OCF_REJECT_CONN_REQ, params, sizeof(params));
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
		bt_emit("connect_ok %s handle=0x%04X\n", addr, handle);
		if (_pending.type == BT_PENDING_PAIR && bt_addr_equal(_pending.addr, dev->addr)) {
			_pending.handle = handle;
			bt_hci_auth_request(handle);
			bt_emit("pair_wait_auth %s\n", addr);
		}
		else if (_pending.type == BT_PENDING_CONNECT && bt_addr_equal(_pending.addr, dev->addr)) {
			bt_clear_pending();
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
		return;
	}

	dev->connected = false;
	dev->handle = 0;
	bt_addr_to_str(dev->addr, addr, sizeof(addr));
	bt_emit("disconnect %s status=%u reason=%u\n", addr, payload[0], payload[3]);
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
	bt_hci_reject_connection(payload);
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
			slog("bluetooth poll event_header_timeout fr=0x%08x\n", get32(UART0_FR_REG));
			return -1;
		}
		if (bt_recv_exact(payload, hdr[1], BT_UART_PKT_FOLLOW_TIMEOUT_MS) != 0) {
			slog("bluetooth poll event_payload_timeout evt=0x%02x len=%u fr=0x%08x\n",
				hdr[0], hdr[1], get32(UART0_FR_REG));
			return -1;
		}
		bt_handle_event(hdr[0], payload, hdr[1]);
		return 1;
	}

	if (pkt_type == HCI_PKT_ACL) {
		++_wait_debug.acl_packets;
		if (bt_recv_exact(hdr, 4, BT_UART_PKT_FOLLOW_TIMEOUT_MS) != 0) {
			slog("bluetooth poll acl_header_timeout fr=0x%08x\n", get32(UART0_FR_REG));
			return -1;
		}
		acl_len = (uint16_t)hdr[2] | ((uint16_t)hdr[3] << 8);
		bt_drop_bytes(acl_len);
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

	while (!_wait_cmd.done && waited < timeout_ms) {
		if (bt_poll_once(1) <= 0) {
			++waited;
		}
	}

	_wait_cmd.active = false;
	if (!_wait_cmd.done) {
		slog("bluetooth wait timeout opcode=0x%04x waited=%u seen=%u evt=%u acl=%u other=%u last_pkt=0x%02x last_evt=0x%02x len=%u last_opcode=0x%04x last_status=%d fr=0x%08x\n",
			opcode, waited, _wait_debug.packets_seen, _wait_debug.event_packets,
			_wait_debug.acl_packets, _wait_debug.other_packets,
			_wait_debug.last_pkt_type, _wait_debug.last_event_code,
			_wait_debug.last_event_len, _wait_debug.last_opcode,
			_wait_debug.last_status, get32(UART0_FR_REG));
	}
	return _wait_cmd.done ? _wait_cmd.status : -1;
}

static int bt_hci_command_sync(uint16_t ogf, uint16_t ocf,
		const uint8_t* params, uint8_t param_len, uint32_t timeout_ms) {
	uint16_t opcode = HCI_OPCODE(ogf, ocf);

	bt_hci_send_command_raw(opcode, params, param_len);
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

	if (bt_hci_command_sync(HCI_OGF_HOST_CTRL, HCI_OCF_RESET, NULL, 0, 1000) != 0) {
		return -1;
	}
	(void)bt_hci_command_sync(HCI_OGF_HOST_CTRL, HCI_OCF_SET_EVENT_MASK, mask, sizeof(mask), 1000);
	(void)bt_hci_command_sync(HCI_OGF_HOST_CTRL, HCI_OCF_WRITE_AUTH_ENABLE, &auth_enable, 1, 1000);
	(void)bt_hci_command_sync(HCI_OGF_HOST_CTRL, HCI_OCF_WRITE_INQUIRY_MODE, &inquiry_mode, 1, 1000);
	(void)bt_hci_command_sync(HCI_OGF_HOST_CTRL, HCI_OCF_WRITE_SIMPLE_PAIRING_MODE, &simple_pair, 1, 1000);
	(void)bt_hci_command_sync(HCI_OGF_HOST_CTRL, HCI_OCF_WRITE_SCAN_ENABLE, &scan_enable, 1, 1000);
	return 0;
}

static int bt_driver_init(void) {
	int ret;
	int flushed;

	_mmio_base = mmio_map();
	if (_mmio_base == 0) {
		slog("bluetooth init mmio_map_failed\n");
		return -1;
	}

	ret = bt_power_on_uart0();
	if (ret != 0) {
		slog("bluetooth init power_on_uart0_failed ret=%d\n", ret);
		return ret;
	}

	bt_enable_gpclk2_32k();
	bt_prepare_combo_chip_power();
	bt_release_bt_shutdown();

	bcm283x_pl011_uart_init_bt();
	flushed = bt_uart_flush();
	slog("bluetooth init pl011 clock=%u ibrd=%u fbrd=%u flushed=%d fr=0x%08x\n",
		bcm283x_pl011_uart_clock_hz(), bcm283x_pl011_uart_ibrd(),
		bcm283x_pl011_uart_fbrd(), flushed, get32(UART0_FR_REG));
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

static void bt_dump_state_ret(char* ret, size_t ret_sz) {
	int i;
	int count = 0;

	for (i = 0; i < MAX_BT_DEVICES; ++i) {
		if (_devices[i].used) {
			++count;
		}
	}
	bt_ret_append(ret, ret_sz, "state ready=%d scanning=%d devices=%d pending=%d\n",
		_ready ? 1 : 0,
		_scanning ? 1 : 0,
		count,
		(int)_pending.type);
}

static void bt_help_emit(void) {
	bt_emit("scan [seconds]\n");
	bt_emit("stop\n");
	bt_emit("devices\n");
	bt_emit("state\n");
	bt_emit("name <bdaddr>\n");
	bt_emit("connect <bdaddr>\n");
	bt_emit("pair <bdaddr> [pin]\n");
	bt_emit("disconnect <bdaddr|handle>\n");
}

static void bt_help_ret(char* ret, size_t ret_sz) {
	bt_ret_append(ret, ret_sz, "scan [seconds]\n");
	bt_ret_append(ret, ret_sz, "stop\n");
	bt_ret_append(ret, ret_sz, "devices\n");
	bt_ret_append(ret, ret_sz, "state\n");
	bt_ret_append(ret, ret_sz, "name <bdaddr>\n");
	bt_ret_append(ret, ret_sz, "connect <bdaddr>\n");
	bt_ret_append(ret, ret_sz, "pair <bdaddr> [pin]\n");
	bt_ret_append(ret, ret_sz, "disconnect <bdaddr|handle>\n");
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
	else if (strcmp(cmd, "state") == 0) {
		bt_dump_state_ret(ret, ret_sz);
	}
	else if (strcmp(cmd, "devices") == 0) {
		bt_list_devices_ret(ret, ret_sz);
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
		if (arg1 == NULL || !bt_parse_addr(arg1, addr)) {
			snprintf(ret, ret_sz, "name_fail reason=bad_addr\n");
			return 0;
		}
		bt_name_request(addr, ret, ret_sz);
	}
	else if (strcmp(cmd, "connect") == 0) {
		if (arg1 == NULL || !bt_parse_addr(arg1, addr)) {
			snprintf(ret, ret_sz, "connect_fail reason=bad_addr\n");
			return 0;
		}
		bt_start_connection(addr, false, NULL, ret, ret_sz);
	}
	else if (strcmp(cmd, "pair") == 0) {
		if (arg1 == NULL || !bt_parse_addr(arg1, addr)) {
			snprintf(ret, ret_sz, "pair_fail reason=bad_addr\n");
			return 0;
		}
		bt_start_connection(addr, true, arg2, ret, ret_sz);
	}
	else if (strcmp(cmd, "disconnect") == 0) {
		if (arg1 == NULL) {
			snprintf(ret, ret_sz, "disconnect_fail reason=missing_target\n");
			return 0;
		}
		bt_disconnect_target(arg1, ret, ret_sz);
	}
	else {
		snprintf(ret, ret_sz, "unknown command\n");
		return 0;
	}
	return 0;
}

static int bt_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
		void* buf, int size, int offset, void* p) {
	int i;

	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)node;
	(void)offset;
	(void)p;

	if (size <= 0) {
		return VFS_ERR_RETRY;
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
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)node;
	(void)p;

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
	strcpy(dev.name, "bluetooth");
	dev.read = bt_read;
	dev.loop_step = bt_loop;
	dev.check_poll_events = bt_check_poll_events;
	dev.cmd = bt_dev_cmd;
	_bt_dev = &dev;

	if (bt_driver_init() != 0) {
		slog("bluetooth error init_failed\n");
	}
	else {
		slog("bluetooth ready classic_hci=1 scan=1 pair=1 connect=1\n");
		bt_help_emit();
	}

	device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666);

	charbuf_free(_evt_buf);
	return 0;
}
