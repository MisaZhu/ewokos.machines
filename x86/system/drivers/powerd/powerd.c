#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ewoksys/vdevice.h>

static int power_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info,
		void* buf, int size, int offset, void* p) {
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)info;
	(void)offset;
	(void)p;
	(void)size;

	static int level = 100;
	uint8_t* data = (uint8_t*)buf;
	data[0] = 1;
	data[1] = 1;
	data[2] = level;
	return 3;
}

int main(int argc, char** argv) {
	const char* mnt_point = argc > 1 ? argv[1] : "/dev/power0";

	vdevice_t dev = {0};
	dev.name[0] = 'p';
	dev.name[1] = 'o';
	dev.name[2] = 'w';
	dev.name[3] = 'e';
	dev.name[4] = 'r';
	dev.name[5] = 'd';
	dev.name[6] = 0;
	dev.read = power_read;
	device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444);
	return 0;
}
