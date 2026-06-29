#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <fbd/fbd.h>
#include <bsp/bsp_gpio.h>
#include <st77xx/st77xx.h>
#include <xpt2046/xpt2046.h>

typedef struct {
	const char* name;
	int lcd_dc;
	int lcd_cs;
	int lcd_rst;
	int lcd_bl;
	uint16_t rot;
	uint16_t inversion;
	uint16_t mode;
	uint32_t color;
} lcd_probe_t;

typedef struct {
	const char* name;
	uint16_t width;
	uint16_t height;
	uint16_t rot;
	uint16_t mode;
	uint32_t color;
} lcd_view_t;

int  do_flush(const void* buf, uint32_t size) {
	st77xx_flush(buf, size);
	return 0;
}

void lcd_init_variant(uint32_t w, uint32_t h, uint32_t div, const lcd_probe_t* cfg) {
	LCD_MODE = cfg->mode;
	/* #region debug-point gnpe-lcdhat24-init-probe */
	klog("lcdhat24: init %s w=%u h=%u div=%u dc=%d cs=%d rst=%d bl=%d rot=%d inv=%d mode=%d\n",
			cfg->name,
			(unsigned int)w, (unsigned int)h, (unsigned int)div,
			cfg->lcd_dc, cfg->lcd_cs, cfg->lcd_rst, cfg->lcd_bl,
			cfg->rot, cfg->inversion, cfg->mode);
	/* #endregion debug-point gnpe-lcdhat24-init-probe */
	st77xx_init(w, h, cfg->rot, cfg->inversion,
			cfg->lcd_dc, cfg->lcd_cs, cfg->lcd_rst, cfg->lcd_bl, div);
	st77xx_set_flush_mode(LCD_FLUSH_FULL);
}

void lcd_init(uint32_t w, uint32_t h, uint32_t div) {
	static const lcd_probe_t cfg = {
		.name = "std-st7789-22-27",
			.lcd_dc = 22,
			.lcd_cs = 8,
			.lcd_rst = 27,
			.lcd_bl = 18,
			.rot = G_ROTATE_0,
			.inversion = 1,
			.mode = LCD_MODE_0,
			.color = 0x000000ff
	};
	lcd_init_variant(w, h, div, &cfg);
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

static int tp_read(uint8_t* buf, uint32_t size) {
	memset(buf, 0, size);
	if(size >= 6) {
		uint16_t* d = (uint16_t*)buf;
		xpt2046_read(&d[0], &d[1], &d[2]);
	}
	return 6;	
}

int main(int argc, char** argv) {
	_spi_div = 4;
	LCD_WIDTH = 240;
	LCD_HEIGHT = 320;

	int opti = doargs(argc, argv);
	const char* mnt_point = (opti < argc && opti >= 0) ? argv[opti]: "/dev/fb0";

	lcd_init(LCD_WIDTH, LCD_HEIGHT, _spi_div);
	proc_usleep(10000);

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
