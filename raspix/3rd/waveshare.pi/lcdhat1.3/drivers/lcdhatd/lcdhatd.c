#include <stdint.h>
#include <st77xx/st77xx.h>

static const int LCD_DC = 25;
static const int LCD_CS = 8;
static const int LCD_RST = 27;
static const int LCD_BL = 24;

void lcd_init(uint32_t w, uint32_t h, uint32_t rot, uint32_t div) {
	st77xx_init(w, h, rot, 1, LCD_DC, LCD_CS, LCD_RST, LCD_BL, div);
}

int do_flush(const void* buf, uint32_t size) {
	st77xx_flush(buf, size);
	return 0;
}
