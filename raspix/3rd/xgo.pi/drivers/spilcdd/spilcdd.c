#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <fb/fb.h>
#include <ewoksys/vdevice.h>
#include <fbd/fbd.h>
#include <st77xx/st77xx.h>

static uint32_t flush(const fbinfo_t* fbinfo, const graph_t* g) {
	(void)fbinfo;
	uint32_t sz = 4 * g->w * g->h;
	st77xx_flush(g->buffer, sz);
	return sz;
}

static fbinfo_t* get_info(void) {
	static fbinfo_t fbinfo;
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

/*LCD_DC	Instruction/Data Register selection
  LCD_CS    LCD chip selection, low active
  LCD_RST   LCD reset
  */
int main(int argc, char** argv) {
	int lcd_rst = 27;
	int lcd_dc = 25;
	int lcd_cs = 8;
	int lcd_bl = 0;

	const char* mnt_point = argc > 1 ? argv[1]: "/dev/fb0";

	if(argc > 4) {
		lcd_rst = atoi(argv[2]);
		lcd_dc = atoi(argv[3]);
		lcd_bl = atoi(argv[4]);
	}

	/*
	 * The original local driver writes MADCTL=0x70 while flushing, which
	 * matches the st77xx shared driver in LCD_MODE_0 + G_ROTATE_90.
	 */
	LCD_MODE = LCD_MODE_0;
	st77xx_init(LCD_WIDTH, LCD_HEIGHT, G_ROTATE_90, 1,
			lcd_dc, lcd_cs, lcd_rst, lcd_bl > 0 ? lcd_bl : -1, 4);

	fbd_t fbd;
	fbd.splash = NULL;
	fbd.flush = flush;
	fbd.init = init;
	fbd.get_info = get_info;
	return fbd_run(&fbd, mnt_point, LCD_WIDTH, LCD_HEIGHT, "");
}
