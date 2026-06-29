#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <bsp/bsp_gpio.h>
#include <bsp/bsp_spi.h>
#include <st77xx/st77xx.h>

static int LCD_DC =	22;
static int LCD_CS	= 8;
static int LCD_RST = 27;
static int LCD_BL = 18;
static int SPI_DIV = 8;

// Screen settings
#define DEF_SCREEN_WIDTH 		320
#define DEF_SCREEN_HEIGHT 		240

// Other define
#define SCREEN_VERTICAL_1		0
#define SCREEN_HORIZONTAL_1		1
#define SCREEN_VERTICAL_2		2
#define SCREEN_HORIZONTAL_2		3

static uint16_t* _lcd_buffer = NULL;
static uint32_t* _shadow_argb = NULL;
static uint8_t _shadow_ready = 0;
uint16_t LCD_WIDTH  = DEF_SCREEN_WIDTH;
uint16_t LCD_HEIGHT = DEF_SCREEN_HEIGHT;
uint16_t LCD_MODE = LCD_MODE_0;

static inline void delay(int32_t count) {
	proc_usleep(count);
}

/* LCD CONTROL */
static inline void lcd_spi_send(uint8_t byte) {
	bsp_spi_transfer(byte);
}

/* Send command (char) to LCD  - OK */
static inline void lcd_write_command(uint8_t command) {
	bsp_gpio_write(LCD_DC, 0);
	lcd_spi_send(command);
	bsp_gpio_write(LCD_DC, 1);
}

/* Send Data (char) to LCD */
static inline void lcd_write_data(uint8_t Data) {
	lcd_spi_send(Data);
}

/* Reset LCD */
static inline void lcd_reset( void ) {
	bsp_gpio_write(LCD_BL,1);

	bsp_gpio_write(LCD_RST, 0);
	delay(200);
	bsp_gpio_write(LCD_RST, 1);
	delay(200);
}

static inline void lcd_start(void) {
	bsp_gpio_write(LCD_CS, 0);
	bsp_spi_activate(1);
}

static inline void lcd_end(void) {
	bsp_spi_activate(0);
	bsp_gpio_write(LCD_CS, 1);
}

/*Ser rotation of the screen - changes x0 and y0*/
static inline void lcd_set_buffer(uint16_t w, uint16_t h, uint16_t rot) {
	uint8_t mod = 0;
	if(LCD_MODE == 0) {
		mod = 0x00;
		switch(rot) {
			case G_ROTATE_90: // 90度
				mod |= 0x70;
				break;
			case G_ROTATE_180: // 180度
				mod |= 0x10;
				break;
			case G_ROTATE_270: // 270度
				mod |= 0xB0;
				break;
			default: //G_ROTATE_0
				mod |= 0x00;
				break;
		}
	}
	else {
		mod = 0x08;
		switch(rot) {
			case G_ROTATE_90: // 90度
				mod |= 0x20;
				break;
			case G_ROTATE_180: // 180度
				mod |= 0x80;
				break;
			case G_ROTATE_270: // 270度
				mod |= 0xf0;
				break;
			default: //G_ROTATE_0
				mod |= 0x40;
				break;
		}
	}

	lcd_write_command(0x36); //MX, MY, RGB mode
    lcd_write_data(mod);
	delay(100);
	LCD_WIDTH = w;
	LCD_HEIGHT = h;
	if(_lcd_buffer != NULL) {
		free(_lcd_buffer);
	}
	if(_shadow_argb != NULL) {
		free(_shadow_argb);
	}
	_lcd_buffer = malloc(LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t));
	_shadow_argb = malloc(LCD_WIDTH * LCD_HEIGHT * sizeof(uint32_t));
	_shadow_ready = 0;
	if(_shadow_argb != NULL) {
		memset(_shadow_argb, 0xff, LCD_WIDTH * LCD_HEIGHT * sizeof(uint32_t));
	}
}

static inline void lcd_brightness(uint8_t brightness) {
	// chyba trzeba wczesniej zainicjalizowac - rejestr 0x53
	lcd_write_command(0x51); // byc moze 2 bajty?
	lcd_write_data(brightness); // byc moze 2 bajty
}

static inline void lcd_set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
	uint16_t x1 = x + w - 1;
	uint16_t y1 = y + h - 1;
	lcd_write_command(0x2A);
	lcd_write_data((x >> 8) & 0xff);
	lcd_write_data(x & 0xff);
	lcd_write_data((x1 >> 8) & 0xff);
	lcd_write_data(x1 & 0xff);

	lcd_write_command(0x2B);
	lcd_write_data((y >> 8) & 0xff);
	lcd_write_data(y & 0xff);
	lcd_write_data((y1 >> 8) & 0xff);
	lcd_write_data(y1 & 0xff);

	lcd_write_command(0x2C); // Memory write?
}

static inline void lcd_show(void) {
	lcd_set_window(0, 0, LCD_WIDTH, LCD_HEIGHT);
	bsp_spi_send_recv((const uint8_t*)_lcd_buffer, NULL,
			(uint32_t)LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t));
}

static inline void lcd_show_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
	uint32_t row_bytes;
	uint32_t row;
	uint16_t* row_ptr;

	if(w == 0 || h == 0) {
		return;
	}

	lcd_set_window(x, y, w, h);
	row_bytes = (uint32_t)w * sizeof(uint16_t);

	if(x == 0 && w == LCD_WIDTH) {
		row_ptr = _lcd_buffer + ((uint32_t)y * LCD_WIDTH);
		bsp_spi_send_recv((const uint8_t*)row_ptr, NULL, row_bytes * h);
		return;
	}

	for(row = 0; row < h; row++) {
		row_ptr = _lcd_buffer + ((uint32_t)(y + row) * LCD_WIDTH) + x;
		bsp_spi_send_recv((const uint8_t*)row_ptr, NULL, row_bytes);
	}
}

static inline uint16_t argb_to_rgb565_be(uint32_t pixel) {
	uint16_t r = (pixel >> 19) & 0x1f;
	uint16_t g = (pixel >> 10) & 0x3f;
	uint16_t b = (pixel >> 3) & 0x1f;
	uint16_t rgb565 = (r << 11) | (g << 5) | b;
	return (rgb565 >> 8) | (rgb565 << 8);
}

void st77xx_flush(const void* buf, uint32_t size) {
	uint32_t *src = (uint32_t*)buf;
	uint32_t x;
	uint32_t y;
	uint32_t idx;
	uint32_t min_x;
	uint32_t min_y;
	uint32_t max_x;
	uint32_t max_y;
	uint8_t dirty;

	if(size < LCD_WIDTH * LCD_HEIGHT * 4 || _lcd_buffer == NULL)
		return;

	if(_shadow_argb == NULL || !_shadow_ready) {
		uint32_t sz = LCD_HEIGHT * LCD_WIDTH;
		uint32_t i;
		for(i = 0; i < sz; i++) {
			_lcd_buffer[i] = argb_to_rgb565_be(src[i]);
		}
		if(_shadow_argb != NULL) {
			memcpy(_shadow_argb, buf, LCD_WIDTH * LCD_HEIGHT * sizeof(uint32_t));
			_shadow_ready = 1;
		}

		bsp_spi_set_div(SPI_DIV);
		lcd_start();
		lcd_show();
		lcd_end();
		return;
	}

	min_x = LCD_WIDTH;
	min_y = LCD_HEIGHT;
	max_x = 0;
	max_y = 0;
	dirty = 0;

	for(y = 0; y < LCD_HEIGHT; y++) {
		for(x = 0; x < LCD_WIDTH; x++) {
			idx = y * LCD_WIDTH + x;
			if(_shadow_argb[idx] == src[idx]) {
				continue;
			}
			_shadow_argb[idx] = src[idx];
			_lcd_buffer[idx] = argb_to_rgb565_be(src[idx]);
			if(!dirty) {
				min_x = max_x = x;
				min_y = max_y = y;
				dirty = 1;
			}
			else {
				if(x < min_x)
					min_x = x;
				if(x > max_x)
					max_x = x;
				if(y < min_y)
					min_y = y;
				if(y > max_y)
					max_y = y;
			}
		}
	}

	if(!dirty) {
		return;
	}

	bsp_spi_set_div(SPI_DIV);
	lcd_start();
	lcd_show_rect(min_x, min_y, max_x - min_x + 1, max_y - min_y + 1);
	lcd_end();
}

void st77xx_init(uint16_t w, uint16_t h, uint16_t rot, uint16_t inversion,
		int pin_dc, int pin_cs, int pin_rst, int pin_bl, int cdiv) {
	LCD_DC = pin_dc;
	LCD_CS = pin_cs;
	LCD_RST = pin_rst;
	LCD_BL = pin_bl;
	bsp_gpio_init();
	bsp_gpio_config(LCD_DC, GPIO_OUTPUT);
	bsp_gpio_config(LCD_CS, GPIO_OUTPUT);
	bsp_gpio_config(LCD_RST, GPIO_OUTPUT);
	bsp_gpio_config(LCD_BL, GPIO_OUTPUT);

	lcd_reset();
	if(cdiv > 0)
		SPI_DIV = cdiv;
	bsp_spi_init();
	bsp_spi_set_div(SPI_DIV);
	bsp_spi_select(SPI_SELECT_0);

	lcd_start();
	lcd_write_command(0x11); 
	delay(120000);
	// lcd_write_command(0x36);
	// lcd_write_data(0x00);

	lcd_write_command(0x3A); 
	lcd_write_data(0x05);

	lcd_write_command(0xB2);
	lcd_write_data(0x0C);
	lcd_write_data(0x0C);
	lcd_write_data(0x00);
	lcd_write_data(0x33);
	lcd_write_data(0x33); 

	lcd_write_command(0xB7); 
	lcd_write_data(0x35);  

	lcd_write_command(0xBB);
	lcd_write_data(0x37);

	lcd_write_command(0xC0);
	lcd_write_data(0x2C);

	lcd_write_command(0xC2);
	lcd_write_data(0x01);

	lcd_write_command(0xC3);
	lcd_write_data(0x12);   

	lcd_write_command(0xC4);
	lcd_write_data(0x20);  

	lcd_write_command(0xC6); 
	lcd_write_data(0x0F);    

	lcd_write_command(0xD0); 
	lcd_write_data(0xA4);
	lcd_write_data(0xA1);

	lcd_write_command(0xE0);
	lcd_write_data(0xD0);
	lcd_write_data(0x04);
	lcd_write_data(0x0D);
	lcd_write_data(0x11);
	lcd_write_data(0x13);
	lcd_write_data(0x2B);
	lcd_write_data(0x3F);
	lcd_write_data(0x54);
	lcd_write_data(0x4C);
	lcd_write_data(0x18);
	lcd_write_data(0x0D);
	lcd_write_data(0x0B);
	lcd_write_data(0x1F);
	lcd_write_data(0x23);

	lcd_write_command(0xE1);
	lcd_write_data(0xD0);
	lcd_write_data(0x04);
	lcd_write_data(0x0C);
	lcd_write_data(0x11);
	lcd_write_data(0x13);
	lcd_write_data(0x2C);
	lcd_write_data(0x3F);
	lcd_write_data(0x44);
	lcd_write_data(0x51);
	lcd_write_data(0x2F);
	lcd_write_data(0x1F);
	lcd_write_data(0x1F);
	lcd_write_data(0x20);
	lcd_write_data(0x23);

	if(inversion == 0)
		lcd_write_command(0x20); // Display Inversion OFF   RPi LCD (A)
	else
		lcd_write_command(0x21); // Display Inversion ON    RPi LCD (B)

	//lcd_write_command(0x36); // Memory Access Control
	//lcd_write_data(0x48);
	lcd_set_buffer(w, h, rot);
	lcd_write_command(0x29); // Display ON
	delay(150000);
	lcd_end();
}
