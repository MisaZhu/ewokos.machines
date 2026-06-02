#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ewoksys/vfs.h>
#include <ewoksys/ipc.h>
#include <ewoksys/klog.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/proc.h>
#include <ewoksys/keydef.h>

#define USB_REPORT_ID_KEYBOARD 2
#define USB_KEY_MOD_LCTRL 0x01
#define USB_KEY_MOD_LSHIFT 0x02
#define USB_KEY_MOD_RCTRL 0x10
#define USB_KEY_MOD_RSHIFT 0x20
#define USB_KEY_ROLLOVER 0x01
#define MAX_KEYS 6

static int _hid_fd = -1;
static uint8_t _keys[MAX_KEYS];
static bool _wakeup = false;
static const char* _hid_dev_point = "/dev/hid0";
static const char* _mnt_point = "/dev/keyb0";

static int set_report_id(int fd, int id) {
	proto_t in;
	int ret;
	PF->init(&in)->addi(&in, id);
	ret = vfs_fcntl(fd, 0, &in, NULL);
	PF->clear(&in);
	return ret;
}

static bool key_exists(const uint8_t* keys, uint8_t num, uint8_t key) {
	for (uint8_t i = 0; i < num; ++i) {
		if (keys[i] == key) {
			return true;
		}
	}
	return false;
}

static uint8_t hid_to_ewok_key(uint8_t hid) {
	if (hid >= 0x04 && hid <= 0x1D) {
		return (uint8_t)('a' + hid - 0x04);
	}
	if (hid >= 0x1E && hid <= 0x26) {
		return (uint8_t)('1' + hid - 0x1E);
	}
	if (hid == 0x27) {
		return '0';
	}

	switch (hid) {
	case 0x28: return KEY_ENTER;
	case 0x29: return KEY_ESC;
	case 0x2A: return KEY_BACKSPACE;
	case 0x2B: return KEY_TAB;
	case 0x2C: return KEY_SPACE;
	case 0x2D: return '-';
	case 0x2E: return '=';
	case 0x2F: return '[';
	case 0x30: return ']';
	case 0x31: return '\\';
	case 0x33: return ';';
	case 0x34: return '\'';
	case 0x35: return '`';
	case 0x36: return ',';
	case 0x37: return '.';
	case 0x38: return '/';
	case 0x4A: return KEY_HOME;
	case 0x4D: return KEY_END;
	case 0x4F: return KEY_RIGHT;
	case 0x50: return KEY_LEFT;
	case 0x51: return KEY_DOWN;
	case 0x52: return KEY_UP;
	case 0x54: return '/';
	case 0x55: return '*';
	case 0x56: return '-';
	case 0x57: return '+';
	case 0x58: return KEY_ENTER;
	case 0x59: return '1';
	case 0x5A: return '2';
	case 0x5B: return '3';
	case 0x5C: return '4';
	case 0x5D: return '5';
	case 0x5E: return '6';
	case 0x5F: return '7';
	case 0x60: return '8';
	case 0x61: return '9';
	case 0x62: return '0';
	case 0x63: return '.';
	default:
		return 0;
	}
}

static void try_open_hid(void) {
	if (_hid_fd >= 0) {
		return;
	}

	_hid_fd = open(_hid_dev_point, O_RDONLY | O_NONBLOCK);
	if (_hid_fd < 0) {
		return;
	}
	if (set_report_id(_hid_fd, USB_REPORT_ID_KEYBOARD) != 0) {
		close(_hid_fd);
		_hid_fd = -1;
		return;
	}
}

static void update_keys(const uint8_t report[8]) {
	uint8_t next[MAX_KEYS] = {0};
	uint8_t count = 0;
	bool changed;

	if ((report[0] & (USB_KEY_MOD_LCTRL | USB_KEY_MOD_RCTRL)) != 0 && count < MAX_KEYS) {
		next[count++] = KEY_CTRL;
	}
	if ((report[0] & USB_KEY_MOD_LSHIFT) != 0 && count < MAX_KEYS) {
		next[count++] = KEY_LSHIFT;
	}
	if ((report[0] & USB_KEY_MOD_RSHIFT) != 0 && count < MAX_KEYS) {
		next[count++] = KEY_RSHIFT;
	}

	for (int i = 2; i < 8 && count < MAX_KEYS; ++i) {
		uint8_t key;
		if (report[i] == 0 || report[i] == USB_KEY_ROLLOVER) {
			continue;
		}
		key = hid_to_ewok_key(report[i]);
		if (key == 0 || key_exists(next, count, key)) {
			continue;
		}
		next[count++] = key;
	}

	changed = memcmp(_keys, next, sizeof(_keys)) != 0;
	if (changed) {
		memcpy(_keys, next, sizeof(_keys));
		_wakeup = true;
	}
}

static int keybd_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info,
		void* buf, int size, int offset, void* p) {
	int num = 0;
	uint8_t* out = (uint8_t*)buf;
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)info;
	(void)offset;
	(void)p;

	for (int i = 0; i < MAX_KEYS && num < size; ++i) {
		if (_keys[i] == 0) {
			continue;
		}
		out[num++] = _keys[i];
	}
	return num > 0 ? num : VFS_ERR_RETRY;
}

static int keybd_loop(vdevice_t* dev, void* p) {
	uint8_t report[8];
	int rd;
	bool had_input = false;
	int budget = 8;
	(void)p;

	if (_hid_fd < 0) {
		try_open_hid();
		proc_usleep(10000);
		return 0;
	}

	ipc_disable();
	while (budget-- > 0 && (rd = read(_hid_fd, report, sizeof(report))) > 0) {
		had_input = true;
		if (rd >= 8) {
			update_keys(report);
		}
		else {
			break;
		}
	}
	ipc_enable();

	if (_wakeup) {
		vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
		_wakeup = false;
	}
	proc_usleep(had_input ? 0 : 1000);
	return 0;
}

int main(int argc, char** argv) {
	const char* mnt_point = argc > 1 ? argv[1] : "/dev/keyb0";
	const char* dev_point = argc > 2 ? argv[2] : "/dev/hid0";
	vdevice_t dev;

	_mnt_point = mnt_point;
	_hid_dev_point = dev_point;
	try_open_hid();

	memset(&dev, 0, sizeof(dev));
	strcpy(dev.name, "usb-keyboard");
	dev.read = keybd_read;
	dev.loop_step = keybd_loop;
	return device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444);
}
