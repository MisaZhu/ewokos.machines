#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <bsp/bsp_spi.h>
#include <displayd/displayd.h>
#include <ili9486/ili9486.h>
#include <xpt2046/xpt2046.h>

static int _lcd_dc_pin = 22;
static int _lcd_cs_pin = 8;
static int _lcd_rst_pin = 27;
static int _lcd_bl_pin = 18;
static int _lcd_spi_div = 4;
static int _lcd_spi_select = SPI_SELECT_0;
static const int _lcd_inversion = 0;

static int _tp_cs_pin = 7;
static int _tp_irq_pin = 17;
static int _tp_spi_div = 64;
static int _tp_spi_select = SPI_SELECT_1;

int  do_flush(const void* buf, uint32_t size) {
	ili9486_flush(buf, size);
	return 0;
}

void lcd_init(uint32_t w, uint32_t h) {
	ili9486_set_config(_lcd_dc_pin, _lcd_cs_pin, _lcd_rst_pin, _lcd_bl_pin,
			_lcd_spi_div, _lcd_spi_select);
	ili9486_init(w, h, G_ROTATE_90, _lcd_inversion,
			_lcd_dc_pin, _lcd_cs_pin, _lcd_rst_pin, _lcd_bl_pin, _lcd_spi_div);
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

const char* _conf_file = "";
static int _display_index = 0;
static int doargs(int argc, char* argv[]) {
	int c = 0;
	while (c != -1) {
		c = getopt (argc, argv, "c:d:i:");
		if(c == -1)
			break;

		switch (c) {
		case 'd':
			_lcd_spi_div = atoi(optarg);
			break;
		case 'c':
			_conf_file = optarg;
			break;
		case 'i':
			_display_index = atoi(optarg);
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
		if(_lcd_cs_pin >= 0)
			bsp_gpio_write(_lcd_cs_pin, 1);
		xpt2046_read(&d[0], &d[1], &d[2]);
		if(_lcd_cs_pin >= 0)
			bsp_gpio_write(_lcd_cs_pin, 0);
	}
	return 6;	
}

int main(int argc, char** argv) {
	uint32_t w=320, h=240;
	LCD_HEIGHT = h;
	LCD_WIDTH = w;

	int opti = doargs(argc, argv);
	const char* mnt_point = (opti < argc && opti >= 0) ? argv[opti]: "/dev/disp0";

	ILI9486_REG_WIDTH_16 = 0;
	ILI9486_INIT_PROFILE = ILI9486_INIT_PROFILE_GENERIC;
	lcd_init(w, h);
	//xpt2046_set_config(_tp_cs_pin, _tp_irq_pin, _tp_spi_div, _tp_spi_select);
	//xpt2046_init(_tp_cs_pin, _tp_irq_pin, _tp_spi_div);

	fbdisplayd_t display;
	memset(&display, 0, sizeof(fbdisplayd_t));
	display.splash = NULL;
	display.flush = flush;
	display.init = init;
	display.get_info = get_info;
	display.read = tp_read;
	int ret = fbdisplayd_run(&display, mnt_point, LCD_WIDTH, LCD_HEIGHT, _conf_file, _display_index);
	return ret;
}
