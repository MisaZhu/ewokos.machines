#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/syscall.h>
#include <ewoksys/keydef.h>
#include <ewoksys/ipc.h>
#include <ewoksys/mmio.h>
#include <ewoksys/vfs.h>
#include <ewoksys/charbuf.h>

#include <arch/rk3506/i2c.h>

#define KEY_PRESS	1
#define KEY_RELEASE	3
#define KEY_TIMEOUT	40

struct kb_state{
	int key;
	uint64_t ts;
};

static struct kb_state kb_states[8] = {0};

static uint8_t key_remap(uint8_t key){
	switch(key){
		case 0xb5:
			return KEY_UP;
		case 0xb6:
			return KEY_DOWN;
		case 0xb4:
			return KEY_LEFT;
		case 0xb7:
			return KEY_RIGHT;
		case 0x0A:
			return KEY_ENTER;
		case 0xB1:
			return KEY_HOME;
		default:
			return key;
	}
}

static bool _ctrl_down = false;

static void do_ctrl(uint8_t c) {
	if(c >= '0' && c <= '9') {
		core_set_active_ux(c - '0');
	}
	else if(c == 19) { //left 
		core_prev_ux();
	}
	else if(c == 4) { //right
		core_next_ux();
	}
}

static charbuf_t *_buffer;

static int kbd_read(int fd, int from_pid, fsinfo_t* node,
		void* buf, int size, int offset, void* p) {
	(void)fd;
	(void)from_pid;
	(void)node;
	(void)offset;
	(void)size;
	(void)p;

	char c;
	int res = charbuf_pop(_buffer, &c);

	if(res != 0)
		return VFS_ERR_RETRY;

	((char*)buf)[0] = c;
	return 1;
}

static int kbd_loop(void* p) {
	uint8_t key[2] = {0};
	bool release_evt = false;
	int ret = rk_i2c_read(0x1f, 0x9,  key, 2, 0);
	if(ret == 0){
		uint8_t c = key[1];
		bool macthed = false;
		if(c >= 0xA1 && c <= 0xA5) {
			//alt: 0xA1, lshift: 0xA2, rshift: 0xA3, ctrl: 0xA5, ?: 0xA4
			if(c == 0xA5) {//ctrl
				if(key[0] == 1)//press
					_ctrl_down = true;
				else if(key[0] == 3)//release
					_ctrl_down = false;
			}
			usleep(20000);
			return -1;
		}
		else {
			c = key_remap(c);
			if(_ctrl_down) {
				do_ctrl(c);
				usleep(20000);
				return -1;
			}
			else {
				for(int i = 0; i < sizeof(kb_states)/sizeof(struct kb_state); i++){
					if(kb_states[i].key == c){
						macthed = true;
						if(key[0] == 1){//press
							kb_states[i].key = c; 
						}
						else if(key[0] == 3){//release
							kb_states[i].key = 0;
							release_evt = true;
						}
						break;
					}
				}
			}
		}

		if(!macthed) {
			for(int i = 0; i < sizeof(kb_states)/sizeof(struct kb_state); i++){
				if(kb_states[i].key == 0){
					if(key[0] == 1){//press
						kb_states[i].key = c;
					}
					else if(key[0] == 3){//release
						kb_states[i].key = 0;
						release_evt = true;
					}
					break;
				}
			}
		}
	}

	bool wake = false;
	for(int i = 0; i < sizeof(kb_states)/sizeof(struct kb_state); i++){
		if(kb_states[i].key != 0){
			charbuf_push(_buffer, kb_states[i].key, true);
			kb_states[i].key = 0;
			wake = true;
		}
	}

	if(!wake && release_evt) {
		charbuf_push(_buffer, 0, true);
		wake = true;
	}

	if(wake)
		proc_wakeup(RW_BLOCK_EVT);
	usleep(20000);
	return 0;
}

int main(int argc, char** argv) {

	const char* mnt_point = argc > 1 ? argv[1]: "/dev/keyb0";
	rk_i2c_init(0);
	memset(kb_states, 0, sizeof(kb_states));
	vdevice_t dev;
	memset(&dev, 0, sizeof(vdevice_t));
	strcpy(dev.name, "keyboard");

	_buffer = charbuf_new(0);

	dev.read = kbd_read;
	dev.loop_step = kbd_loop;
	device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444);

	charbuf_free(_buffer);
	return 0;
}
