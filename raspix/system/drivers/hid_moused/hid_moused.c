#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ewoksys/vfs.h>
#include <ewoksys/ipc.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/mmio.h>
#include <mouse/mouse.h>
#include <fcntl.h>

#define CACHE_SIZE (32)

/* exponential idle backoff: poll the hid device fast while reports flow,
   back off to a slow cadence when it stays silent to stop burning CPU */
#define HID_IDLE_SLEEP_MIN_US 3000u
#define HID_IDLE_SLEEP_MAX_US 50000u

static int hid;
static uint32_t _idle_sleep_us = HID_IDLE_SLEEP_MIN_US;
static mouse_evt_t mouse_data[CACHE_SIZE];
static uint32_t mouse_data_read = 0;
static uint32_t mouse_data_write = 0;
static uint8_t last_btn = 0;

static void mouse_push_evt(uint8_t state, uint8_t button, int16_t x, int16_t y) {
	if (mouse_data_write - mouse_data_read >= CACHE_SIZE) {
		mouse_data_read++;
	}
	mouse_evt_t* evt = &mouse_data[mouse_data_write % CACHE_SIZE];
	memset(evt, 0, sizeof(mouse_evt_t));
	evt->type = MOUSE_TYPE_REL;
	evt->state = state;
	evt->button = button;
	evt->x = x;
	evt->y = y;
	mouse_data_write++;
}

static uint8_t hid_btn_to_mouse(uint8_t mask) {
	if (mask & 0x01)
		return MOUSE_BUTTON_LEFT;
	if (mask & 0x02)
		return MOUSE_BUTTON_RIGHT;
	if (mask & 0x04)
		return MOUSE_BUTTON_MID;
	return MOUSE_BUTTON_NONE;
}

static void mouse_handle_report(uint8_t btn, int8_t dx, int8_t dy, int8_t wheel) {
	uint8_t pressed = btn & (uint8_t)~last_btn;
	uint8_t released = last_btn & (uint8_t)~btn;
	last_btn = btn;

	if (pressed) {
		mouse_push_evt(MOUSE_STATE_DOWN, hid_btn_to_mouse(pressed), dx, dy);
	}
	else if (released) {
		mouse_push_evt(MOUSE_STATE_UP, hid_btn_to_mouse(released), dx, dy);
	}
	else if (dx != 0 || dy != 0) {
		mouse_push_evt(MOUSE_STATE_MOVE, MOUSE_BUTTON_NONE, dx, dy);
	}

	if (wheel > 0) {
		mouse_push_evt(MOUSE_STATE_MOVE, MOUSE_BUTTON_SCROLL_DOWN, 0, 0);
	}
	else if (wheel < 0) {
		mouse_push_evt(MOUSE_STATE_MOVE, MOUSE_BUTTON_SCROLL_UP, 0, 0);
	}
}

static int _read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
		void* buf, int size, int offset, void* p) {
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)offset;
	(void)p;
	(void)node;

	if (size < (int)sizeof(mouse_evt_t))
		return -1;

	if (mouse_data_write - mouse_data_read > 0) {
		memcpy(buf, &mouse_data[mouse_data_read % CACHE_SIZE], sizeof(mouse_evt_t));
		memset(&mouse_data[mouse_data_read % CACHE_SIZE], 0, sizeof(mouse_evt_t));
		mouse_data_read++;
		return sizeof(mouse_evt_t);
	}
	return VFS_ERR_RETRY;
}

static uint32_t mouse_check_poll_events(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node, void* p) {
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)node;
	(void)p;

	if (mouse_data_write - mouse_data_read > 0) {
		return VFS_EVT_RD;
	}
	return 0;
}

static int _loop(vdevice_t* dev, void* p) {
	(void)p;

	ipc_disable();

	bool wakeup = false;
	while(true) {
		uint8_t buf[8] = {0};
		int res = read(hid, buf, 7);
		if(res == 7) {
			/* payload: buttons, dx, dy, wheel (deltas are signed) */
			mouse_handle_report(buf[0], (int8_t)buf[1], (int8_t)buf[2], (int8_t)buf[3]);
			wakeup = true;
		}
		else {
			break;
		}
	}

	ipc_enable();
	/*
	 * Level-triggered wakeup: re-assert VFS_EVT_RD whenever the ring
	 * still has unread events, not only on the edge when a new report
	 * arrives.  The libgloss _read() loop clears the sticky RD bit
	 * (vfs_clear_poll_events) before vfs_block(), and vfs_block() only
	 * prechecks sticky bits — so if the edge was consumed while events
	 * remain queued, a blocked reader (xmouse) would sleep forever
	 * until the next physical mouse movement.
	 */
	if(mouse_data_write - mouse_data_read > 0) {
		_idle_sleep_us = HID_IDLE_SLEEP_MIN_US;
		vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
	}
	else if(wakeup) {
		_idle_sleep_us = HID_IDLE_SLEEP_MIN_US;
	}
	else {
		usleep(_idle_sleep_us);
		if (_idle_sleep_us < HID_IDLE_SLEEP_MAX_US) {
			_idle_sleep_us <<= 1;
			if (_idle_sleep_us > HID_IDLE_SLEEP_MAX_US) {
				_idle_sleep_us = HID_IDLE_SLEEP_MAX_US;
			}
		}
	}
	return 0;
}

static int set_report_id(int fd, int id) {

	proto_t in;
	PF->init(&in)->addi(&in, id);
	int ret = vfs_fcntl(fd, 0, &in , NULL);
	PF->clear(&in);
	return ret;
}

int main(int argc, char** argv) {
	const char* mnt_point = argc > 1 ? argv[1]: "/dev/mouse0";
	const char* dev_point = argc > 2 ? argv[2]: "/dev/hid0";
	hid = open(dev_point, O_RDONLY | O_NONBLOCK);
	if (hid < 0) {
		return -1;
	}
	if (set_report_id(hid, 1) != 0) {
		return -1;
	}

	vdevice_t dev;
	memset(&dev, 0, sizeof(vdevice_t));
	strcpy(dev.name, "mouse");
	dev.loop_step = _loop;
	dev.read = _read;
	dev.check_poll_events = mouse_check_poll_events;
	device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444);
	return 0;
}
