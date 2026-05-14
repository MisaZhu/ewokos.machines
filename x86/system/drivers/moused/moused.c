#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <ewoksys/vfs.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/interrupt.h>
#include <ewoksys/klog.h>
#include <mouse/mouse.h>
#include <bsp/x86_pio.h>

#define I8042_DATA_PORT 0x60
#define I8042_STATUS_PORT 0x64
#define I8042_CMD_PORT 0x64

#define I8042_STATUS_OBF 0x01
#define I8042_STATUS_IBF 0x02
#define I8042_STATUS_AUX 0x20

#define I8042_CMD_READ_CFG 0x20
#define I8042_CMD_WRITE_CFG 0x60
#define I8042_CMD_ENABLE_AUX 0xA8
#define I8042_CMD_AUX_WRITE 0xD4

#define I8042_CFG_AUX_IRQ 0x02
#define I8042_CFG_AUX_CLK_DISABLE 0x20

#define PS2_CMD_SET_DEFAULT 0xF6
#define PS2_CMD_SET_SAMPLE_RATE 0xF3
#define PS2_CMD_GET_DEVICE_ID 0xF2
#define PS2_CMD_ENABLE_REPORT 0xF4
#define PS2_ACK 0xFA

#define IRQ_MOUSE 12
#define MOUSE_CACHE 64
#define I8042_WAIT_LOOPS 5000

static mouse_evt_t mouse_data[MOUSE_CACHE];
static int mouse_r = 0;
static int mouse_w = 0;
static uint8_t packet[4];
static uint8_t packet_index = 0;
static uint8_t packet_size = 3;
static uint8_t button_state = 0;
static bool _wakeup = false;
static bool _polling_mode = false;

static int i8042_wait_write(void) {
	for (int i = 0; i < I8042_WAIT_LOOPS; ++i) {
		if ((x86_inb(I8042_STATUS_PORT) & I8042_STATUS_IBF) == 0) {
			return 0;
		}
		io_wait();
	}
	return -1;
}

static int i8042_wait_read(void) {
	for (int i = 0; i < I8042_WAIT_LOOPS; ++i) {
		if ((x86_inb(I8042_STATUS_PORT) & I8042_STATUS_OBF) != 0) {
			return 0;
		}
		io_wait();
	}
	return -1;
}

static int i8042_wait_aux_read(void) {
	for (int i = 0; i < I8042_WAIT_LOOPS; ++i) {
		uint8_t status = x86_inb(I8042_STATUS_PORT);
		if ((status & (I8042_STATUS_OBF | I8042_STATUS_AUX)) ==
				(I8042_STATUS_OBF | I8042_STATUS_AUX)) {
			return 0;
		}
		io_wait();
	}
	return -1;
}

static void i8042_flush(void) {
	for (int i = 0; i < 64; ++i) {
		if ((x86_inb(I8042_STATUS_PORT) & I8042_STATUS_OBF) == 0) {
			break;
		}
		(void)x86_inb(I8042_DATA_PORT);
	}
}

static int i8042_write_cmd(uint8_t value) {
	if (i8042_wait_write() != 0) {
		return -1;
	}
	x86_outb(I8042_CMD_PORT, value);
	return 0;
}

static int i8042_write_data(uint8_t value) {
	if (i8042_wait_write() != 0) {
		return -1;
	}
	x86_outb(I8042_DATA_PORT, value);
	return 0;
}

static int i8042_read_data(uint8_t* value) {
	if (i8042_wait_read() != 0) {
		return -1;
	}
	*value = x86_inb(I8042_DATA_PORT);
	return 0;
}

static int i8042_read_aux_data(uint8_t* value) {
	if (i8042_wait_aux_read() != 0) {
		return -1;
	}
	*value = x86_inb(I8042_DATA_PORT);
	return 0;
}

static int i8042_read_mouse_reply(uint8_t* value) {
	if (i8042_read_aux_data(value) == 0) {
		return 0;
	}
	return i8042_read_data(value);
}

static int i8042_read_cfg(uint8_t* value) {
	if (i8042_write_cmd(I8042_CMD_READ_CFG) != 0) {
		return -1;
	}
	return i8042_read_data(value);
}

static int i8042_write_cfg(uint8_t value) {
	if (i8042_write_cmd(I8042_CMD_WRITE_CFG) != 0) {
		return -1;
	}
	return i8042_write_data(value);
}

static int ps2_mouse_write(uint8_t value) {
	uint8_t ack;

	if (i8042_write_cmd(I8042_CMD_AUX_WRITE) != 0) {
		return -1;
	}
	if (i8042_write_data(value) != 0) {
		return -1;
	}
	if (i8042_read_mouse_reply(&ack) != 0) {
		return -1;
	}
	return ack == PS2_ACK ? 0 : -1;
}

static int ps2_mouse_set_rate(uint8_t value) {
	if (ps2_mouse_write(PS2_CMD_SET_SAMPLE_RATE) != 0) {
		return -1;
	}
	return ps2_mouse_write(value);
}

static int ps2_mouse_get_id(uint8_t* id) {
	uint8_t ack;

	if (i8042_write_cmd(I8042_CMD_AUX_WRITE) != 0) {
		return -1;
	}
	if (i8042_write_data(PS2_CMD_GET_DEVICE_ID) != 0) {
		return -1;
	}
	if (i8042_read_mouse_reply(&ack) != 0 || ack != PS2_ACK) {
		return -1;
	}
	return i8042_read_mouse_reply(id);
}

static void mouse_enqueue(const mouse_evt_t* evt) {
	mouse_data[mouse_w] = *evt;
	mouse_w = (mouse_w + 1) % MOUSE_CACHE;
	if (mouse_w == mouse_r) {
		mouse_r = (mouse_r + 1) % MOUSE_CACHE;
	}
	_wakeup = true;
}

static int8_t mouse_wheel_delta(uint8_t value) {
	int8_t wheel = value & 0x0F;
	if ((wheel & 0x08) != 0) {
		wheel |= 0xF0;
	}
	return wheel;
}

static int mouse_button_from_mask(uint8_t mask) {
	if (mask == 0x1) {
		return MOUSE_BUTTON_LEFT;
	}
	if (mask == 0x2) {
		return MOUSE_BUTTON_RIGHT;
	}
	if (mask == 0x4) {
		return MOUSE_BUTTON_MID;
	}
	return MOUSE_BUTTON_NONE;
}

static void mouse_handle_packet(void) {
	if ((packet[0] & 0xC0) != 0) {
		return;
	}

	mouse_evt_t evt;
	memset(&evt, 0, sizeof(evt));
	evt.type = MOUSE_TYPE_REL;
	evt.x = (int8_t)packet[1];
	evt.y = -(int8_t)packet[2];
	evt.button = MOUSE_BUTTON_NONE;
	evt.state = MOUSE_STATE_MOVE;

	uint8_t buttons = packet[0] & 0x07;
	uint8_t changed = buttons ^ button_state;
	button_state = buttons;

	if (packet_size == 4) {
		int8_t wheel = mouse_wheel_delta(packet[3]);
		if (wheel > 0) {
			mouse_evt_t wheel_evt = evt;
			wheel_evt.button = MOUSE_BUTTON_SCROLL_UP;
			mouse_enqueue(&wheel_evt);
		}
		else if (wheel < 0) {
			mouse_evt_t wheel_evt = evt;
			wheel_evt.button = MOUSE_BUTTON_SCROLL_DOWN;
			mouse_enqueue(&wheel_evt);
		}
	}

	if (changed != 0) {
		uint8_t mask = changed & 0x1;
		if (mask == 0) {
			mask = changed & 0x2;
		}
		if (mask == 0) {
			mask = changed & 0x4;
		}
		evt.button = mouse_button_from_mask(mask);
		evt.state = (buttons & mask) != 0 ? MOUSE_STATE_DOWN : MOUSE_STATE_UP;
		mouse_enqueue(&evt);
		return;
	}

	if (evt.x != 0 || evt.y != 0) {
		mouse_enqueue(&evt);
	}
}

static void mouse_process_byte(uint8_t value) {
	if (packet_index == 0 && (value & 0x08) == 0) {
		return;
	}

	packet[packet_index++] = value;
	if (packet_index >= packet_size) {
		packet_index = 0;
		mouse_handle_packet();
	}
}

static void mouse_poll_input(void) {
	for (int i = 0; i < 32; ++i) {
		uint8_t status = x86_inb(I8042_STATUS_PORT);
		if ((status & (I8042_STATUS_OBF | I8042_STATUS_AUX)) !=
				(I8042_STATUS_OBF | I8042_STATUS_AUX)) {
			break;
		}
		mouse_process_byte(x86_inb(I8042_DATA_PORT));
	}
}

static int mouse_init(void) {
	uint8_t cfg;
	uint8_t id = 0;
	bool have_cfg = false;

	memset(mouse_data, 0, sizeof(mouse_data));
	i8042_flush();

	if (i8042_read_cfg(&cfg) == 0) {
		have_cfg = true;
		cfg |= I8042_CFG_AUX_IRQ;
		cfg &= ~I8042_CFG_AUX_CLK_DISABLE;
		if (i8042_write_cfg(cfg) != 0) {
			klog("moused: write cfg fail\n");
			have_cfg = false;
		}
	}
	else {
		klog("moused: read cfg skip\n");
	}
	if (i8042_write_cmd(I8042_CMD_ENABLE_AUX) != 0) {
		klog("moused: enable aux fail\n");
		return -1;
	}
	if (ps2_mouse_write(PS2_CMD_SET_DEFAULT) != 0) {
		klog("moused: set default fail\n");
		return -1;
	}

	if (ps2_mouse_set_rate(200) == 0 &&
			ps2_mouse_set_rate(100) == 0 &&
			ps2_mouse_set_rate(80) == 0 &&
			ps2_mouse_get_id(&id) == 0 &&
			(id == 3 || id == 4)) {
		packet_size = 4;
	}
	else {
		packet_size = 3;
	}

	if (ps2_mouse_write(PS2_CMD_ENABLE_REPORT) != 0) {
		klog("moused: enable report fail\n");
		return -1;
	}

	if (!have_cfg) {
		/* Some QEMU setups do not expose the controller config byte to userland. */
		_polling_mode = true;
		klog("moused: run without cfg byte\n");
	}
	else {
		_polling_mode = false;
	}
	i8042_flush();
	return 0;
}

static void mouse_interrupt_handle(uint32_t interrupt, uint32_t data) {
	(void)interrupt;
	(void)data;

	uint8_t status = x86_inb(I8042_STATUS_PORT);
	if ((status & I8042_STATUS_OBF) == 0 || (status & I8042_STATUS_AUX) == 0) {
		return;
	}

	mouse_process_byte(x86_inb(I8042_DATA_PORT));
}

static int moused_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info,
		void* buf, int size, int offset, void* p) {
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)info;
	(void)size;
	(void)offset;
	(void)p;

	if (mouse_r == mouse_w) {
		return VFS_ERR_RETRY;
	}

	memcpy(buf, &mouse_data[mouse_r], sizeof(mouse_evt_t));
	mouse_r = (mouse_r + 1) % MOUSE_CACHE;
	return sizeof(mouse_evt_t);
}

static int mouse_loop(struct st_vdevice* dev, void* p) {
	(void)p;
	if (_polling_mode) {
		mouse_poll_input();
	}
	if (_wakeup) {
		vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
		_wakeup = false;
	}
	usleep(1000);
	return 0;
}

int main(int argc, char** argv) {
	const char* mnt_point = argc > 1 ? argv[1] : "/dev/mouse0";

	if (mouse_init() != 0) {
		klog("moused: init failed\n");
		return -1;
	}

	vdevice_t dev;
	memset(&dev, 0, sizeof(vdevice_t));
	strcpy(dev.name, "mouse");
	dev.read = moused_read;
	dev.loop_step = mouse_loop;

	static interrupt_handler_t handler;
	handler.data = 0;
	handler.handler = mouse_interrupt_handle;
	sys_interrupt_setup(IRQ_MOUSE, &handler);

	device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444);
	return 0;
}
