#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <fbd/fbd.h>

static uint32_t LCD_HEIGHT = 480;
static uint32_t LCD_WIDTH = 280;


int  do_flush(const void* buf, uint32_t size);
void lcd_init(uint32_t w, uint32_t h, uint32_t rot, uint32_t div);


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
static int doargs(int argc, char* argv[]) {
	int c = 0;
	while (c != -1) {
		c = getopt (argc, argv, "d:");
		if(c == -1)
			break;

		switch (c) {
		case 'd':
			_spi_div = atoi(optarg);
			break;
		default:
			c = -1;
			break;
		}
	}
	return optind;
}

int main(int argc, char** argv) {
	_spi_div = 4;
	uint32_t h=480, w=280;
	LCD_HEIGHT = h;
	LCD_WIDTH = w;

	int opti = doargs(argc, argv);
	const char* mnt_point = (opti < argc && opti >= 0) ? argv[opti]: "/dev/fb0";

	lcd_init(w, h, G_ROTATE_NONE, _spi_div);

	fbd_t fbd;
	fbd.splash = NULL;
	fbd.flush = flush;
	fbd.init = init;
	fbd.get_info = get_info;
	int ret = fbd_run(&fbd, mnt_point, LCD_WIDTH, LCD_HEIGHT, "");
	return ret;
}
