#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ewoksys/vfs.h>
#include <ewoksys/klog.h>
#include <ewoksys/ipc.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/mmio.h>
#include <ewoksys/charbuf.h>
#include <ewoksys/syscall.h>
#include <ewoksys/proc.h>
#include <ewoksys/interrupt.h>
#include <ewoksys/timer.h>
#include <fcntl.h>
#include <ewoksys/keydef.h>

#define KEY_MOD_LCTRL  0x01
#define KEY_MOD_LSHIFT 0x02
#define KEY_MOD_LALT   0x04
#define KEY_MOD_LMETA  0x08
#define KEY_MOD_RCTRL  0x10
#define KEY_MOD_RSHIFT 0x20
#define KEY_MOD_RALT   0x40
#define KEY_MOD_RMETA  0x80

#define MAX_KEY (5) /* payload carries 5 keycodes: mod, reserved, key[5] */

static int hid;

/* current held-key state, refreshed by each HID report snapshot */
static uint8_t _mod = 0;
static uint8_t _keys[MAX_KEY];
static int _key_count = 0;

const char downMap[] = {  
        ' ',' ',' ',' ','a','b','c','d',    'e','f','g','h','i','j','k','l',
        'm','n','o','p','q','r','s','t',    'u','v','w','x','y','z','1','2',
        '3','4','5','6','7','8','9','0',    '\r','\x1b','\b','\t','\x20', '-', '=', '[', 
        ']', '\\', '$', ';', '\'', '`',',','.',     '/',
    };

const char upMap[] = {
        ' ',' ',' ',' ','A','B','C','D',    'E','F','G','H','I','J','K','L',
        'M','N','O','P','Q','R','S','T',    'U','V','W','X','Y','Z','!','@',
        '#','$','%','^','&','*','(',')',    '\r','\x1b','\b','\t','\x20', '_', '+', '{', 
        '}', '|', '$', ':', '\"', '~','<','>',      '?',
};

static uint8_t do_ctrl(char c) {
	return c;
}

uint8_t getKeyChar(uint8_t alt, uint8_t keycode){
    if(keycode > 0 && keycode < sizeof(upMap)){
        if((alt & KEY_MOD_LCTRL) ||(alt & KEY_MOD_RCTRL)){
        	return do_ctrl(downMap[keycode]);
		}
        if((alt & KEY_MOD_LSHIFT) ||(alt & KEY_MOD_RSHIFT)){
            return upMap[keycode];
        }else{
        	return downMap[keycode];
        }
    }else if(keycode == 0x4f){
		return KEY_RIGHT;
	}else if(keycode == 0x50){
		return KEY_LEFT;
	}else if(keycode == 0x51){
		return KEY_DOWN;
	}else if(keycode == 0x52){
		return KEY_UP;
	}
    return 0;
}

static int get_key_code(char *buf, int size) {
	int num = 0;
	for (int i = 0; i < _key_count && num < size; i++) {
		uint8_t c = getKeyChar(_mod, _keys[i]);
		if (c != 0) {
			buf[num++] = c;
		}
	}
	return num;
}

static int keyb_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node, 
		void* buf, int size, int offset, void* p) {
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)offset;
	(void)p;
	(void)node;

	int num = get_key_code(buf, size);
	return num ? num : VFS_ERR_RETRY;
}

static uint32_t keyb_check_poll_events(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node, void* p) {
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)node;
	(void)p;

	return _key_count > 0 ? VFS_EVT_RD : 0;
}

static int loop(vdevice_t* dev, void* p) {
	(void)p;

	ipc_disable();

	bool changed = false;
	while(true) {
		uint8_t buf[8] = {0};
		int res = read(hid, buf, 7);
		if(res == 7) {
			/* each report is a full snapshot: mod, reserved, keycodes */
			uint8_t keys[MAX_KEY];
			int count = 0;
			for (int i = 2; i < 7; i++) {
				if (buf[i] != 0) {
					keys[count++] = buf[i];
				}
			}
			if (buf[0] != _mod || count != _key_count ||
					memcmp(keys, _keys, count) != 0) {
				changed = true;
			}
			_mod = buf[0];
			_key_count = count;
			memcpy(_keys, keys, sizeof(keys));
		}
		else {
			break;
		}
	}

	ipc_enable();
	if(changed)
		vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
	else
		proc_usleep(3000);
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
	const char* mnt_point = argc > 1 ? argv[1]: "/dev/keyb0";
	const char* dev_point = argc > 2 ? argv[2]: "/dev/hid0";
	hid = open(dev_point, O_RDONLY | O_NONBLOCK);
	if (hid < 0) {
		return -1;
	}
	if (set_report_id(hid, 2) != 0) {
		return -1;
	}

	vdevice_t dev;
	memset(&dev, 0, sizeof(vdevice_t));
	strcpy(dev.name, "keyb");
	dev.read = keyb_read;
	dev.loop_step = loop;
	dev.check_poll_events = keyb_check_poll_events;

	device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444);
	return 0;
}
