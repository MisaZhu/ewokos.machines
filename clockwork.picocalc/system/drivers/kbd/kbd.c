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
#include <ewoksys/keydef.h>

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

static int kbd_read(int fd, int from_pid, fsinfo_t* node,
		void* buf, int size, int offset, void* p) {
	(void)fd;
	(void)from_pid;
	(void)node;
	(void)offset;
	(void)size;
	(void)p;

	uint8_t key[2] = {0};
	uint64_t now = kernel_tic_ms(0); 
	int ret = rk_i2c_read(0x1f, 0x9,  key, 2, 0);
	if(ret == 0){
		bool macthed = false;
		key[1] = key_remap(key[1]);
		for(int i = 0; i < sizeof(kb_states)/sizeof(struct kb_state); i++){
			if(kb_states[i].key == key[1]){
				macthed = true;
				if(key[0] == 1){//press
					kb_states[i].key = key[1];
					kb_states[i].ts = now;
				}
				else if(key[0] == 3){//release
					kb_states[i].key = 0;
				}
				break;
			}
		}

		if(!macthed) {
			for(int i = 0; i < sizeof(kb_states)/sizeof(struct kb_state); i++){
				if(kb_states[i].key == 0){
					if(key[0] == 1){//press
						kb_states[i].key = key[1];
						kb_states[i].ts = now;
					}
					else if(key[0] == 3){//release
						kb_states[i].key = 0;
					}
					break;
				}
			}
		}
	}

	int cnt = 0;
	for(int i = 0; i < sizeof(kb_states)/sizeof(struct kb_state); i++){
		if(kb_states[i].key != 0){
			/*if(now - kb_states[i].ts >= KEY_TIMEOUT){
				kb_states[i].key = 0;
				continue;
			}
			*/
			*(uint8_t*)buf++ = kb_states[i].key;	
			cnt++;
		}
	}
	return cnt?cnt:-1;
}

int main(int argc, char** argv) {

	const char* mnt_point = argc > 1 ? argv[1]: "/dev/keyb0";
	rk_i2c_init(0);
	memset(kb_states, 0, sizeof(kb_states));
	vdevice_t dev;
	memset(&dev, 0, sizeof(vdevice_t));
	strcpy(dev.name, "keyboard");
	dev.read = kbd_read;
	device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444);
	return 0;
}
