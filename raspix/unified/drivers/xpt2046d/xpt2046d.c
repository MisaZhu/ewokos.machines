#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <fbd/fbd.h>
#include <xpt2046/xpt2046.h>
#include <ewoksys/vdevice.h>

static int _spi_div = 128;
static int _tp_cs = 7;
static int _tp_irq = 25;

static int doargs(int argc, char* argv[]) {
	int c = 0;
	while (c != -1) {
		c = getopt (argc, argv, "c:i:d:");
		if(c == -1)
			break;

		switch (c) {
		case 'd':
			_spi_div = atoi(optarg);
			break;
		case 'c':
			_tp_cs = atoi(optarg);
			break;
		case 'i':
			_tp_irq = atoi(optarg);
			break;
		default:
			c = -1;
			break;
		}
	}
	return optind;
}

static int tp_read(int fd, int from_pid, fsinfo_t* node,
		void* buf, int size, int offset, void* p) {
	(void)fd;
	(void)from_pid;
	(void)node;
	(void)offset;
	(void)p;

	memset(buf, 0, size);
	if(size >= 6) {
		uint16_t* d = (uint16_t*)buf;
		bsp_gpio_write(8, 1);
		xpt2046_read(&d[0], &d[1], &d[2]);
		bsp_gpio_write(8, 0);
		//klog("tp_read: %d %d %d\n", d[0], d[1], d[2]);
	}
	return 6;	
}

int main(int argc, char** argv) {
	_spi_div = 128;
	_tp_cs = 7;
	_tp_irq = 25;

	int opti = doargs(argc, argv);
	const char* mnt_point = (opti < argc && opti >= 0) ? argv[opti]: "/dev/touch0";

	bsp_gpio_init();
	bsp_spi_init();

	xpt2046_init(_tp_cs, _tp_irq, _spi_div);

	vdevice_t dev;
	memset(&dev, 0, sizeof(vdevice_t));
	strcpy(dev.name, "xpt2046");
	dev.read = tp_read;

	device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444);
	return 0;
}
