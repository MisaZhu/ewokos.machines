#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <ewoksys/vfs.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/interrupt.h>
#include <bsp/x86_pio.h>
#include "keymap.h"

#define I8042_DATA_PORT 0x60
#define I8042_STATUS_PORT 0x64
#define I8042_CMD_PORT 0x64

#define I8042_STATUS_OBF 0x01
#define I8042_STATUS_IBF 0x02
#define I8042_STATUS_AUX 0x20

#define I8042_CMD_READ_CFG 0x20
#define I8042_CMD_WRITE_CFG 0x60
#define I8042_CMD_ENABLE_KBD 0xAE

#define I8042_CFG_KBD_IRQ 0x01
#define I8042_CFG_KBD_CLK_DISABLE 0x10

#define KBD_CMD_SET_DEFAULT 0xF6
#define KBD_CMD_ENABLE_SCAN 0xF4
#define KBD_ACK 0xFA

#define IRQ_KEYBD 1
#define MAX_KEY 6
#define I8042_WAIT_LOOPS 100000

typedef struct {
	uint8_t key_count;
	uint8_t key_code[MAX_KEY];
} key_data_t;

static key_data_t key_data;
static bool _extended = false;
static bool _wakeup = false;

static int i8042_wait_write(void) {
	for (int i = 0; i < I8042_WAIT_LOOPS; ++i) {
		if ((x86_inb(I8042_STATUS_PORT) & I8042_STATUS_IBF) == 0) {
			return 0;
		}
	}
	return -1;
}

static int i8042_wait_read(void) {
	for (int i = 0; i < I8042_WAIT_LOOPS; ++i) {
		if ((x86_inb(I8042_STATUS_PORT) & I8042_STATUS_OBF) != 0) {
			return 0;
		}
	}
	return -1;
}

static void i8042_flush(void) {
	for (int i = 0; i < 32; ++i) {
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

static int keybd_write(uint8_t value) {
	uint8_t ack;

	if (i8042_write_data(value) != 0) {
		return -1;
	}
	if (i8042_read_data(&ack) != 0) {
		return -1;
	}
	return ack == KBD_ACK ? 0 : -1;
}

static int keybd_init(void) {
	uint8_t cfg;

	memset(&key_data, 0, sizeof(key_data));
	i8042_flush();

	if (i8042_read_cfg(&cfg) != 0) {
		return -1;
	}
	cfg |= I8042_CFG_KBD_IRQ;
	cfg &= ~I8042_CFG_KBD_CLK_DISABLE;
	if (i8042_write_cfg(cfg) != 0) {
		return -1;
	}
	if (i8042_write_cmd(I8042_CMD_ENABLE_KBD) != 0) {
		return -1;
	}
	if (keybd_write(KBD_CMD_SET_DEFAULT) != 0) {
		return -1;
	}
	if (keybd_write(KBD_CMD_ENABLE_SCAN) != 0) {
		return -1;
	}
	i8042_flush();
	return 0;
}

static uint8_t translate_scancode(uint8_t scancode, bool extended) {
	uint8_t index = scancode;

	if (extended) {
		switch (scancode) {
		case 0x1C: index = 0x60; break;
		case 0x1D: index = 0x61; break;
		case 0x35: index = 0x62; break;
		case 0x47: index = 0x66; break;
		case 0x48: index = 0x67; break;
		case 0x4B: index = 0x69; break;
		case 0x4D: index = 0x6A; break;
		case 0x4F: index = 0x6B; break;
		case 0x50: index = 0x6C; break;
		default:
			return 0;
		}
	}

	if (index >= sizeof(keymap)) {
		return 0;
	}
	return keymap[index];
}

static void key_press(uint8_t key_code) {
	for (uint8_t i = 0; i < key_data.key_count; ++i) {
		if (key_data.key_code[i] == key_code) {
			return;
		}
	}
	if (key_data.key_count < MAX_KEY) {
		key_data.key_code[key_data.key_count++] = key_code;
	}
}

static void key_release(uint8_t key_code) {
	for (uint8_t i = 0; i < key_data.key_count; ++i) {
		if (key_data.key_code[i] == key_code) {
			for (uint8_t j = i; j + 1 < key_data.key_count; ++j) {
				key_data.key_code[j] = key_data.key_code[j + 1];
			}
			key_data.key_count--;
			return;
		}
	}
}

static void keybd_interrupt_handle(uint32_t interrupt, uint32_t data) {
	(void)interrupt;
	(void)data;

	uint8_t status = x86_inb(I8042_STATUS_PORT);
	if ((status & I8042_STATUS_OBF) == 0 || (status & I8042_STATUS_AUX) != 0) {
		return;
	}

	uint8_t raw = x86_inb(I8042_DATA_PORT);
	if (raw == 0xE0) {
		_extended = true;
		return;
	}
	if (raw == 0xE1 || raw == KBD_ACK) {
		_extended = false;
		return;
	}

	bool release = (raw & 0x80) != 0;
	uint8_t key = translate_scancode(raw & 0x7F, _extended);
	_extended = false;
	if (key == 0) {
		return;
	}

	if (release) {
		key_release(key);
	}
	else {
		key_press(key);
	}
	_wakeup = true;
}

static int keybd_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info,
		void* buf, int size, int offset, void* p) {
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)info;
	(void)offset;
	(void)p;

	uint8_t* out = (uint8_t*)buf;
	int num = 0;
	for (uint8_t i = 0; i < key_data.key_count && num < size; ++i) {
		out[num++] = key_data.key_code[i];
	}
	return num > 0 ? num : VFS_ERR_RETRY;
}

static int keybd_loop(struct st_vdevice* dev, void* p) {
	(void)p;
	if (_wakeup) {
		vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
		_wakeup = false;
	}
	usleep(3000);
	return 0;
}

int main(int argc, char** argv) {
	const char* mnt_point = argc > 1 ? argv[1] : "/dev/keyb0";

	if (keybd_init() != 0) {
		return -1;
	}

	vdevice_t dev;
	memset(&dev, 0, sizeof(vdevice_t));
	strcpy(dev.name, "keyboard");
	dev.read = keybd_read;
	dev.loop_step = keybd_loop;

	static interrupt_handler_t handler;
	handler.data = 0;
	handler.handler = keybd_interrupt_handle;
	sys_interrupt_setup(IRQ_KEYBD, &handler);

	device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444);
	return 0;
}
