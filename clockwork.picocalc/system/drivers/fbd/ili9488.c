#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <arch/rk3506/spi.h>
#include <arch/rk3506/gpio.h>
#include "ili9488.h"


#define LCD_RST 2
#define LCD_DC	3

static uint8_t *_fb;
static uint32_t *_shadow_argb;
static int _rectangle[4];

static inline void sleep_ms(int ms){
    proc_usleep(ms * 1000);
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
    unsigned char coord[4];
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

void ili9488_clear(uint32_t color) {
    int i;
    uint8_t rgb[3];

    rgb[0] = color >> 24;  
    rgb[1] = color >> 16;
    rgb[2] = color >> 8;
    define_region_spi(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, 1);
    rk_gpio_write(LCD_DC, 1);
    for (i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
           rk_spi_write(rgb, 3);
    }

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

static void update_shadow(int x, int y, int w, int h, const uint32_t *argb) {
	int row;

	if (_shadow_argb == NULL) {
		return;
	}

	for (row = 0; row < h; row++) {
		uint32_t *shadow_row = _shadow_argb + (y + row) * LCD_WIDTH + x;
		const uint32_t *src_row = argb + row * w;
		memcpy(shadow_row, src_row, (size_t)w * sizeof(uint32_t));
	}
}

void ili9488_draw(int x, int y, int w, int h, uint32_t *argb){
    int i;
    uint8_t *p = _fb;
    const uint32_t *src = argb;

	if (argb == NULL || w <= 0 || h <= 0) {
		return;
	}

	if (region_unchanged(x, y, w, h, src)) {
		return;
	}

	define_region_spi(x, y, x + w - 1, y + h - 1, 1);
    rk_gpio_write(LCD_DC, 1);
    for (i = 0; i < w * h; i++) {
		uint32_t pixel = *src++;
		*p++ = (pixel >> 16) & 0xff;
		*p++ = (pixel >> 8) & 0xff;
		*p++ = pixel & 0xff;
    }

    rk_spi_write(_fb, w * h * 3);
	update_shadow(x, y, w, h, argb);
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

	_fb = dma_alloc(0, LCD_WIDTH * LCD_HEIGHT * 3);
	_shadow_argb = malloc(LCD_WIDTH * LCD_HEIGHT * sizeof(uint32_t));
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
    spi_write_data(0x66); // 18/24-bit colour for SPI (RGB666/RGB888)

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
