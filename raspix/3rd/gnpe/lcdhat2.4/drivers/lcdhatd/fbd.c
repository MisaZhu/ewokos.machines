#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <fbd/fbd.h>
#include <ili9341/ili9341.h>
#include <xpt2046/xpt2046.h>

int  do_flush(const void* buf, uint32_t size) {
	ili9341_flush(buf, size);
	return 0;
}

static void show_test_pattern(uint32_t w, uint32_t h) {
	uint32_t* buf = malloc(w * h * 4);
	if(buf == NULL) {
		return;
	}

	for(uint32_t y = 0; y < h; y++) {
		for(uint32_t x = 0; x < w; x++) {
			uint32_t c;
			if(x < (w / 3)) {
				c = 0x00ff0000;
			}
			else if(x < (w * 2 / 3)) {
				c = 0x0000ff00;
			}
			else {
				c = 0x000000ff;
			}

			if(x < 4 || y < 4 || x >= (w - 4) || y >= (h - 4)) {
				c = 0x00ffffff;
			}
			buf[y * w + x] = c;
		}
	}

	do_flush(buf, w * h * 4);
	free(buf);
}

void lcd_init(uint32_t w, uint32_t h, uint32_t div) {
	const int lcd_dc = 22;
	const int lcd_cs = 8;
	const int lcd_rst = 27;
	const int lcd_bl = -1;
	ili9341_init(w, h, G_ROTATE_270, 0, lcd_dc, lcd_cs, lcd_rst, lcd_bl, div);
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

static int _spi_div = 16;
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
		xpt2046_read(&d[0], &d[1], &d[2]);
	}
	return 6;	
}

int main(int argc, char** argv) {
	_spi_div = 16;
	LCD_HEIGHT = 240;
	LCD_WIDTH = 320;

	int opti = doargs(argc, argv);
	const char* mnt_point = (opti < argc && opti >= 0) ? argv[opti]: "/dev/fb0";

	lcd_init(LCD_WIDTH, LCD_HEIGHT, _spi_div);
	show_test_pattern(LCD_WIDTH, LCD_HEIGHT);
	proc_usleep(300000);

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
