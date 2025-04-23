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
static uint8_t _keys[4]  = {0};
static uint8_t _key_num = 0;

/*alt: 0xA1, lshift: 0xA2, rshift: 0xA3, ctrl: 0xA5, ?: 0xA4
home: 0xD2, del: 0xD4, end: 0xD5, brk: 0xD8, esc: 0xB1
*/
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
			return KEY_ESC;
		case 0xD2:
			return KEY_HOME;
		case 0xD5:
			return KEY_END;
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

static int kbd_read(int fd, int from_pid, fsinfo_t* node,
		void* buf, int size, int offset, void* p) {
	(void)fd;
	(void)from_pid;
	(void)node;
	(void)offset;
	(void)p;

	int ret = 0;
	for(int i=0; i<_key_num && i <size; i++) {
		if(_keys[i] != 0) {
			*(uint8_t*)buf++ = _keys[i];
			ret++;
		}
	}
	return ret ? ret:-1;
}

static int kbd_loop(void*) {
	uint8_t key[2] = {0};
	int ret = rk_i2c_read(0x1f, 0x9,  key, 2, 0);
	if(ret == 0){
		uint8_t c = key[1];
		bool macthed = false;
		if((c >= 0xA1 && c <= 0xA5) || c == KEY_HOME) {
			if(c == 0xA5) {//ctrl
				if(key[0] == 1)//press
					_ctrl_down = true;
				else if(key[0] == 3)//release
					_ctrl_down = false;
			}
			return -1;
		}
		else {
			c = key_remap(c);
			if(_ctrl_down) {
				do_ctrl(c);
				usleep(20000);
				return -1;
			}

			for(int i = 0; i < sizeof(kb_states)/sizeof(struct kb_state); i++){
				if(kb_states[i].key == c){
					macthed = true;
					if(key[0] == 1){//press
						kb_states[i].key = c; 
					}
					else if(key[0] == 3){//release
						kb_states[i].key = 0;
					}
					break;
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
					}
					break;
				}
			}
		}
	}

	_key_num = 0;
	memset(_keys, 0, 4);
	for(int i = 0; i < sizeof(kb_states)/sizeof(struct kb_state); i++){
		if(kb_states[i].key != 0){
			_keys[_key_num] = kb_states[i].key;	
			_key_num++;
			if(_key_num >= 4)
				break;
		}
	}
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

	dev.read = kbd_read;
	dev.loop_step = kbd_loop;
	device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444);
	return 0;
}
