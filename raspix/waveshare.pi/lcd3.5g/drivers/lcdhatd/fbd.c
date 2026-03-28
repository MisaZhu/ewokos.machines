#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <fbd/fbd.h>
#include <graph/graph.h>
#include <ili9486/ili9486.h>
#include <xpt2046/xpt2046.h>

int  do_flush(const void* buf, uint32_t size) {
	ili9486_flush(buf, size);
	return 0;
}

void lcd_init(uint32_t w, uint32_t h, uint32_t div) {
	const int lcd_dc = 22;
	const int lcd_cs = 8;
	const int lcd_rst = 27;
	const int lcd_bl = 18;
	ili9486_init(w, h, G_ROTATE_270, 1, lcd_dc, lcd_cs, lcd_rst, lcd_bl, div);
}

static uint32_t flush(const fbinfo_t* fbinfo, const graph_t* g) {
	uint32_t sz = 4 * g->w * g->h;
	do_flush(g->buffer, sz);
	return sz;
}

static fbinfo_t* get_info(void) {
	static fbinfo_t fbinfo;
	memset(&fbinfo, 0, sizeof(fbinfo_t));
	fbinfo.width = LCD_WIDTH;
	fbinfo.height = LCD_HEIGHT;
	fbinfo.depth = 32;
	return &fbinfo;
}

static int32_t init(uint32_t w, uint32_t h, uint32_t dep) {
	(void)w;
	(void)h;
	(void)dep;
	return 0;
}

static int _spi_div = 8;
const char* _conf_file = "";
static int doargs(int argc, char* argv[]) {
	int c = 0;
	while (c != -1) {
		c = getopt (argc, argv, "c:d:");
		if(c == -1)
			break;

		switch (c) {
		case 'd':
			_spi_div = atoi(optarg);
			break;
		case 'c':
			_conf_file = optarg;
			break;
		default:
			c = -1;
			break;
		}
	}
	return optind;
}

static int tp_read(uint8_t* buf, uint32_t size) {
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
	_spi_div = 4;
	uint32_t w=480, h=320;
	LCD_HEIGHT = h;
	LCD_WIDTH = w;

	int opti = doargs(argc, argv);
	const char* mnt_point = (opti < argc && opti >= 0) ? argv[opti]: "/dev/fb0";

	lcd_init(w, h, _spi_div);

	const int tp_cs = 7;
	const int tp_irq = 17;
	xpt2046_init(tp_cs, tp_irq, 64);

	fbd_t fbd;
	fbd.splash = NULL;
	fbd.flush = flush;
	fbd.init = init;
	fbd.get_info = get_info;
	fbd.read = tp_read;
	int ret = fbd_run(&fbd, mnt_point, LCD_WIDTH, LCD_HEIGHT, _conf_file);
	return ret;
}
