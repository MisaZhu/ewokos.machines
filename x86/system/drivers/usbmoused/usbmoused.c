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
#include <mouse/mouse.h>

#define USB_REPORT_ID_MOUSE 1
#define MOUSE_CACHE 64

static int _hid_fd = -1;
static mouse_evt_t _events[MOUSE_CACHE];
static int _evt_r = 0;
static int _evt_w = 0;
static uint8_t _buttons = 0;
static bool _wakeup = false;
static const char* _hid_dev_point = "/dev/hid0";
static const char* _mnt_point = "/dev/mouse0";

static int set_report_id(int fd, int id) {
	proto_t in;
	int ret;
	PF->init(&in)->addi(&in, id);
	ret = vfs_fcntl(fd, 0, &in, NULL);
	PF->clear(&in);
	return ret;
}

static void try_open_hid(void) {
	if (_hid_fd >= 0) {
		return;
	}

	_hid_fd = open(_hid_dev_point, O_RDONLY | O_NONBLOCK);
	if (_hid_fd < 0) {
		return;
	}
	if (set_report_id(_hid_fd, USB_REPORT_ID_MOUSE) != 0) {
		close(_hid_fd);
		_hid_fd = -1;
		return;
	}
}

static void enqueue_event(const mouse_evt_t* evt) {
	_events[_evt_w] = *evt;
	_evt_w = (_evt_w + 1) % MOUSE_CACHE;
	if (_evt_w == _evt_r) {
		_evt_r = (_evt_r + 1) % MOUSE_CACHE;
	}
	_wakeup = true;
}

static uint8_t button_from_mask(uint8_t mask) {
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

static int8_t wheel_delta(uint8_t value) {
	return (int8_t)value;
}

static void handle_report(const uint8_t report[4]) {
	mouse_evt_t evt;
	uint8_t buttons = (uint8_t)(report[0] & 0x07);
	uint8_t changed = (uint8_t)(buttons ^ _buttons);
	int8_t dx = (int8_t)report[1];
	int8_t dy = (int8_t)report[2];
	int8_t wheel;

	if ((report[0] & 0xC0) != 0) {
		return;
	}

	wheel = wheel_delta(report[3]);

	memset(&evt, 0, sizeof(evt));
	evt.type = MOUSE_TYPE_REL;
	evt.x = dx;
	evt.y = dy;
	evt.button = MOUSE_BUTTON_NONE;
	evt.state = MOUSE_STATE_MOVE;

	if (wheel > 0) {
		mouse_evt_t scroll = evt;
		scroll.state = MOUSE_STATE_MOVE;
		scroll.button = MOUSE_BUTTON_SCROLL_UP;
		enqueue_event(&scroll);
	}
	else if (wheel < 0) {
		mouse_evt_t scroll = evt;
		scroll.state = MOUSE_STATE_MOVE;
		scroll.button = MOUSE_BUTTON_SCROLL_DOWN;
		enqueue_event(&scroll);
	}

	if (changed != 0) {
		uint8_t mask = changed & 0x1;
		if (mask == 0) {
			mask = changed & 0x2;
		}
		if (mask == 0) {
			mask = changed & 0x4;
		}
		evt.button = button_from_mask(mask);
		evt.state = (buttons & mask) != 0 ? MOUSE_STATE_DOWN : MOUSE_STATE_UP;
		_buttons = buttons;
		enqueue_event(&evt);
		return;
	}

	_buttons = buttons;
	if (dx != 0 || dy != 0) {
		evt.state = MOUSE_STATE_MOVE;
		evt.button = MOUSE_BUTTON_NONE;
		enqueue_event(&evt);
	}
}

static int usbmoused_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info,
		void* buf, int size, int offset, void* p) {
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)info;
	(void)size;
	(void)offset;
	(void)p;

	if (_evt_r == _evt_w) {
		return VFS_ERR_RETRY;
	}
	memcpy(buf, &_events[_evt_r], sizeof(mouse_evt_t));
	_evt_r = (_evt_r + 1) % MOUSE_CACHE;
	return sizeof(mouse_evt_t);
}

static int mouse_loop(vdevice_t* dev, void* p) {
	uint8_t report[4];
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
		if (rd >= 3) {
			if (rd < 4) {
				report[3] = 0;
			}
			handle_report(report);
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
	const char* mnt_point = argc > 1 ? argv[1] : "/dev/mouse0";
	const char* dev_point = argc > 2 ? argv[2] : "/dev/hid0";
	vdevice_t dev;

	_mnt_point = mnt_point;
	_hid_dev_point = dev_point;
	try_open_hid();

	memset(&dev, 0, sizeof(dev));
	strcpy(dev.name, "usb-mouse");
	dev.read = usbmoused_read;
	dev.loop_step = mouse_loop;
	return device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444);
}
