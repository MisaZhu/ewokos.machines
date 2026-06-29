#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <fbd/fbd.h>
#include <ili9341/ili9341.h>

int  do_flush(const void* buf, uint32_t size) {
	ili9341_flush(buf, size);
	return 0;
}

static void show_test_pattern(uint32_t w, uint32_t h) {
	static uint32_t buf[320 * 240];

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
}

void lcd_init(uint32_t w, uint32_t h, uint32_t div) {
	const int lcd_dc = 25;
	const int lcd_cs = 8;
	const int lcd_rst = 24;
	const int lcd_bl = 18;
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

static int _spi_div = 64;
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

int main(int argc, char** argv) {
	_spi_div = 8;
	LCD_HEIGHT = 240;
	LCD_WIDTH = 320;

	int opti = doargs(argc, argv);
	const char* mnt_point = (opti < argc && opti >= 0) ? argv[opti]: "/dev/fb0";

	lcd_init(LCD_WIDTH, LCD_HEIGHT, _spi_div);

	fbd_t fbd;
	fbd.splash = NULL;
	fbd.flush = flush;
	fbd.init = init;
	fbd.get_info = get_info;
	int ret = fbd_run(&fbd, mnt_point, LCD_WIDTH, LCD_HEIGHT, _conf_file);
	return ret;
}
