#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <arch/rk3506/spi.h>
#include <arch/rk3506/gpio.h>
#include "ili9488.h"


#define LCD_RST 2
#define LCD_DC	3

static uint16_t *_fb;
static uint32_t *_shadow_argb;
static int _rectangle[4];
static uint8_t _shadow_ready;

static inline void sleep_ms(int ms){
    proc_usleep(ms * 1000);
}

static inline void spi_set_byte_mode(void) {
	rk_spi_set_bits_per_word(8);
}

static inline void spi_set_pixel_mode(void) {
	rk_spi_set_bits_per_word(16);
}

static inline void spi_write_data(uint8_t data){
    rk_gpio_write(LCD_DC, 1);
    rk_spi_write(&data, 1);
}

static inline uint8_t  spi_read_data(uint8_t reg){
	uint8_t ret;
	rk_gpio_write(LCD_DC, 0);
	rk_spi_write(&reg, 1);
	rk_spi_read(&ret, 1);
	return ret;
}

static inline void spi_write_command(uint8_t data){
    rk_gpio_write(LCD_DC, 0);
    rk_spi_write(&data, 1);
}

static inline void spi_write_cd(uint8_t command, int len, ...) {
    int i;
    va_list ap;
    va_start(ap, len);
    spi_write_command(command);
    for (i = 0; i < len; i++) spi_write_data((char) va_arg(ap, int));
    va_end(ap);
}

static inline void define_region_spi(int xstart, int ystart, int xend, int yend, int rw) {
	(void)rw;
	spi_set_byte_mode();
	if(xstart != _rectangle[0] || ystart != _rectangle[1] || xend != _rectangle[2] || yend != _rectangle[3]){
		_rectangle[0] = xstart;
		_rectangle[1] = ystart;
		_rectangle[2] = xend;
		_rectangle[3] = yend;
		spi_write_command(0x2A);
		spi_write_data(xstart >> 8);
		spi_write_data(xstart & 0xFF); 
		spi_write_data(xend >> 8);
		spi_write_data(xend & 0xFF);
		spi_write_command(0x2B); 
		spi_write_data(ystart>>8);
		spi_write_data(ystart &0xff);     
		spi_write_data(yend>>8);
		spi_write_data(yend &0xff);
	}
	spi_write_command(0x2C);
}

static inline uint16_t argb_to_rgb565(uint32_t pixel) {
	uint16_t r = (pixel >> 19) & 0x1f;
	uint16_t g = (pixel >> 10) & 0x3f;
	uint16_t b = (pixel >> 3) & 0x1f;
	uint16_t rgb565 = (r << 11) | (g << 5) | b;
	return (rgb565 >> 8) | (rgb565 << 8);
}

void ili9488_clear(uint32_t color) {
    int i;
	uint16_t rgb565 = argb_to_rgb565(color);

    define_region_spi(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, 1);
    rk_gpio_write(LCD_DC, 1);
	spi_set_pixel_mode();
    for (i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
		_fb[i] = rgb565;
    }
	rk_spi_write((uint8_t*)_fb, LCD_WIDTH * LCD_HEIGHT * (int)sizeof(uint16_t));
	spi_set_byte_mode();

}

static int region_unchanged(int x, int y, int w, int h, const uint32_t *argb) {
	int row;

	if (_shadow_argb == NULL) {
		return 0;
	}

	for (row = 0; row < h; row++) {
		const uint32_t *shadow_row = _shadow_argb + (y + row) * LCD_WIDTH + x;
		const uint32_t *src_row = argb + row * w;
		if (memcmp(shadow_row, src_row, (size_t)w * sizeof(uint32_t)) != 0) {
			return 0;
		}
	}
	return 1;
}

static void update_shadow(int x, int y, int w, int h, const uint32_t *argb, int stride) {
	int row;

	if (_shadow_argb == NULL) {
		return;
	}

	for (row = 0; row < h; row++) {
		uint32_t *shadow_row = _shadow_argb + (y + row) * LCD_WIDTH + x;
		const uint32_t *src_row = argb + row * stride;
		memcpy(shadow_row, src_row, (size_t)w * sizeof(uint32_t));
	}
}

static int find_dirty_rect(int x, int y, int w, int h, const uint32_t *argb,
		int *out_x, int *out_y, int *out_w, int *out_h) {
	int row;
	int col;
	int min_x = w;
	int min_y = h;
	int max_x = -1;
	int max_y = -1;

	if (_shadow_argb == NULL || !_shadow_ready) {
		*out_x = x;
		*out_y = y;
		*out_w = w;
		*out_h = h;
		return 1;
	}

	for (row = 0; row < h; row++) {
		const uint32_t *shadow_row = _shadow_argb + (y + row) * LCD_WIDTH + x;
		const uint32_t *src_row = argb + row * w;
		for (col = 0; col < w; col++) {
			if (shadow_row[col] == src_row[col]) {
				continue;
			}
			if (col < min_x)
				min_x = col;
			if (col > max_x)
				max_x = col;
			if (row < min_y)
				min_y = row;
			if (row > max_y)
				max_y = row;
		}
	}

	if (max_x < min_x || max_y < min_y) {
		return 0;
	}

	*out_x = x + min_x;
	*out_y = y + min_y;
	*out_w = max_x - min_x + 1;
	*out_h = max_y - min_y + 1;
	return 1;
}

void ili9488_draw(int x, int y, int w, int h, uint32_t *argb){
    int row;
    int col;
    int dirty_x;
    int dirty_y;
    int dirty_w;
    int dirty_h;
    uint16_t *p = _fb;
    const uint32_t *src;

	if (argb == NULL || w <= 0 || h <= 0) {
		return;
	}

	if (!find_dirty_rect(x, y, w, h, argb, &dirty_x, &dirty_y, &dirty_w, &dirty_h)) {
		return;
	}

	src = argb + (dirty_y - y) * w + (dirty_x - x);

	define_region_spi(dirty_x, dirty_y, dirty_x + dirty_w - 1, dirty_y + dirty_h - 1, 1);
    rk_gpio_write(LCD_DC, 1);
	spi_set_pixel_mode();
	for (row = 0; row < dirty_h; row++) {
		const uint32_t *src_row = src + row * w;
		for (col = 0; col < dirty_w; col++) {
			*p++ = argb_to_rgb565(src_row[col]);
		}
	}

    rk_spi_write((uint8_t*)_fb, dirty_w * dirty_h * (int)sizeof(uint16_t));
	spi_set_byte_mode();
	update_shadow(dirty_x, dirty_y, dirty_w, dirty_h, src, w);
	_shadow_ready = 1;
}

void ili9488_init(void){
	rk_gpio_init();
	rk_spi_init();
	rk_gpio_config(LCD_RST , 1);
	rk_gpio_config(LCD_DC,	1);

	rk_gpio_write(LCD_RST, 0);
    proc_usleep(100);
	rk_gpio_write(LCD_RST, 1);
	proc_usleep(10000);

	_fb = dma_alloc(0, LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t));
	_shadow_argb = malloc(LCD_WIDTH * LCD_HEIGHT * sizeof(uint32_t));
	_shadow_ready = 0;
	if (_shadow_argb != NULL) {
		memset(_shadow_argb, 0xff, LCD_WIDTH * LCD_HEIGHT * sizeof(uint32_t));
	}

	spi_write_command(0xE0); // Positive Gamma Control
    spi_write_data(0x00);
    spi_write_data(0x03);
    spi_write_data(0x09);
    spi_write_data(0x08);
    spi_write_data(0x16);
    spi_write_data(0x0A);
    spi_write_data(0x3F);
    spi_write_data(0x78);
    spi_write_data(0x4C);
    spi_write_data(0x09);
    spi_write_data(0x0A);
    spi_write_data(0x08);
    spi_write_data(0x16);
    spi_write_data(0x1A);
    spi_write_data(0x0F);

    spi_write_command(0XE1); // Negative Gamma Control
    spi_write_data(0x00);
    spi_write_data(0x16);
    spi_write_data(0x19);
    spi_write_data(0x03);
    spi_write_data(0x0F);
    spi_write_data(0x05);
    spi_write_data(0x32);
    spi_write_data(0x45);
    spi_write_data(0x46);
    spi_write_data(0x04);
    spi_write_data(0x0E);
    spi_write_data(0x0D);
    spi_write_data(0x35);
    spi_write_data(0x37);
    spi_write_data(0x0F);

    spi_write_command(0XC0); // Power Control 1
    spi_write_data(0x17);
    spi_write_data(0x15);

    spi_write_command(0xC1); // Power Control 2
    spi_write_data(0x41);

    spi_write_command(0xC5); // VCOM Control
    spi_write_data(0x00);
    spi_write_data(0x12);
    spi_write_data(0x80);

    spi_write_command(TFT_MADCTL); // Memory Access Control
    spi_write_data(0x48); // MX, BGR

    spi_write_command(0x3A); // Pixel Interface Format
    spi_write_data(0x55); // 16-bit RGB565 for SPI

    spi_write_command(0xB0); // Interface Mode Control
    spi_write_data(0x00);

    spi_write_command(0xB1); // Frame Rate Control
    spi_write_data(0xA0);

    spi_write_command(TFT_INVON);

    spi_write_command(0xB4); // Display Inversion Control
    spi_write_data(0x02);

    spi_write_command(0xB6); // Display Function Control
    spi_write_data(0x02);
    spi_write_data(0x02);
    spi_write_data(0x3B);

    spi_write_command(0xB7); // Entry Mode Set
    spi_write_data(0xC6);
    spi_write_command(0xE9);
    spi_write_data(0x00);

    spi_write_command(0xF7); // Adjust Control 3
    spi_write_data(0xA9);
    spi_write_data(0x51);
    spi_write_data(0x2C);
    spi_write_data(0x82);

    spi_write_command(TFT_SLPOUT); //Exit Sleep
    sleep_ms(120);

    spi_write_command(TFT_DISPON); //Display on
    sleep_ms(120);

    spi_write_command(TFT_MADCTL);
    spi_write_cd(ILI9341_MEMCONTROL, 1, ILI9341_Portrait);

	ili9488_clear(0x0);
}
