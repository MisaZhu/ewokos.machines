#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <ewoksys/vfs.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/ipc.h>

#define HID_TOUCH_REPORT_ID 3
#define HID_IDLE_SLEEP_MIN_US 1000u
#define HID_IDLE_SLEEP_MAX_US 4000u
#define TOUCH_CACHE_SIZE 128u

static int hid = -1;
static uint16_t touch_data[TOUCH_CACHE_SIZE][3];
static uint32_t touch_data_read = 0;
static uint32_t touch_data_write = 0;
static uint32_t idle_sleep_us = HID_IDLE_SLEEP_MIN_US;

static bool touch_has_data(void) {
        return (touch_data_write - touch_data_read) > 0;
}

static void touch_push(uint16_t state, uint16_t x, uint16_t y) {
        if (touch_data_write - touch_data_read >= TOUCH_CACHE_SIZE) {
                touch_data_read++;
        }

        uint16_t* evt = touch_data[touch_data_write % TOUCH_CACHE_SIZE];
        evt[0] = state;
        evt[1] = x;
        evt[2] = y;
        touch_data_write++;
}

static int touch_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
		void* buf, int size, int offset, void* p) {
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)node;
	(void)offset;
	(void)p;

        if (!touch_has_data()) {
		return VFS_ERR_RETRY;
	}
	if (size < 6) {
		return -1;
	}

        memcpy(buf, touch_data[touch_data_read % TOUCH_CACHE_SIZE], 6);
        memset(touch_data[touch_data_read % TOUCH_CACHE_SIZE], 0, sizeof(touch_data[0]));
        touch_data_read++;
	return 6;
}

static int set_report_id(int fd, int id) {
	proto_t in;
	PF->init(&in)->addi(&in, id);
	int ret = vfs_fcntl(fd, 0, &in, NULL);
	PF->clear(&in);
	return ret;
}

static uint32_t touch_check_poll_events(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node, void* p) {
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)node;
	(void)p;

        return touch_has_data() ? VFS_EVT_RD : 0;
}

static int touch_loop(vdevice_t* dev, void* p) {
	uint8_t buf[8] = {0};
        bool got = false;
	(void)p;

	ipc_disable();
	while (true) {
		int ret = read(hid, buf, 7);
		if (ret != 7) {
			break;
		}
                touch_push((uint16_t)buf[0],
                                (uint16_t)buf[1] | ((uint16_t)buf[2] << 8),
                                (uint16_t)buf[3] | ((uint16_t)buf[4] << 8));
                got = true;
	}
	ipc_enable();

        if (touch_has_data()) {
		idle_sleep_us = HID_IDLE_SLEEP_MIN_US;
		vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
	}
        else if (got) {
                idle_sleep_us = HID_IDLE_SLEEP_MIN_US;
        }
	else {
		usleep(idle_sleep_us);
		if (idle_sleep_us < HID_IDLE_SLEEP_MAX_US) {
			idle_sleep_us <<= 1;
			if (idle_sleep_us > HID_IDLE_SLEEP_MAX_US) {
				idle_sleep_us = HID_IDLE_SLEEP_MAX_US;
			}
		}
	}
	return 0;
}

int main(int argc, char** argv) {
	const char* mnt_point = argc > 1 ? argv[1] : "/dev/touch0";
	const char* dev_point = argc > 2 ? argv[2] : "/dev/hid0";

        /*
         * usbhostd may not have enumerated and registered the touch interface
         * yet when hid_touchd starts. Keep retrying so /dev/touch0 does not
         * disappear permanently after an early boot race.
         */
        while (1) {
                hid = open(dev_point, O_RDONLY | O_NONBLOCK);
                if (hid >= 0 && set_report_id(hid, HID_TOUCH_REPORT_ID) == 0) {
                        break;
                }
                if (hid >= 0) {
                        close(hid);
                        hid = -1;
                }
                proc_usleep(200000);
	}

	vdevice_t dev;
	memset(&dev, 0, sizeof(dev));
	strcpy(dev.name, "touch");
	dev.loop_step = touch_loop;
	dev.read = touch_read;
	dev.check_poll_events = touch_check_poll_events;
	return device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444);
}
