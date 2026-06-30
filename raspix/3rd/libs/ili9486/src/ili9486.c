#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ewoksys/kernel_tic.h>
#include <bsp/bsp_gpio.h>
#include <bsp/bsp_spi.h>
#include <ili9486/ili9486.h>

static int LCD_DC =	24;
static int LCD_CS	= 8;
static int LCD_RST = 25;
static int LCD_BL = 18;
static int SPI_DIV = 2;

// Screen settings
#define DEF_SCREEN_WIDTH 		320
#define DEF_SCREEN_HEIGHT 		240

// Other define
#define SCREEN_VERTICAL_1		0
#define SCREEN_HORIZONTAL_1		1
#define SCREEN_VERTICAL_2		2
#define SCREEN_HORIZONTAL_2		3
#define FLUSH_MIN_INTERVAL_MS 16
#define DIRTY_AREA_FULL_THRESHOLD 3  /* dirty_area > total/THRESHOLD => full refresh */

static uint16_t* _lcd_buffer = NULL;
static uint32_t* _shadow_argb = NULL;
static uint8_t _shadow_ready = 0;
static uint16_t LCD_ROT = G_ROTATE_0;
static uint16_t LCD_FLUSH_MODE = LCD_FLUSH_PARTIAL;
static uint8_t _pending_flush = 0;
static uint64_t _last_flush_ms = 0;

/* accumulated dirty rect for deferred flushes */
static uint32_t _pend_min_x = 0;
static uint32_t _pend_max_x = 0;
static uint32_t _pend_min_y = 0;
static uint32_t _pend_max_y = 0;
uint16_t LCD_WIDTH  = DEF_SCREEN_WIDTH;
uint16_t LCD_HEIGHT = DEF_SCREEN_HEIGHT;
int ILI9486_REG_WIDTH_16 = 0;

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
	if(ILI9486_REG_WIDTH_16)
		lcd_spi_send(0x00);
	lcd_spi_send(command);
	bsp_gpio_write(LCD_DC, 1);
}

/* Send Data (char) to LCD */
static inline void lcd_write_data(uint8_t Data) {
	if(ILI9486_REG_WIDTH_16)
		lcd_spi_send(0x00);
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
	LCD_ROT = rot;
	lcd_write_command(0x36);
	switch(rot) {
        case G_ROTATE_0: // 0度
            lcd_write_data(0x08);
            break;
        case G_ROTATE_90: // 90度
            lcd_write_data(0x28);
            break;
        case G_ROTATE_180: // 180度
            lcd_write_data(0xC8);
            break;
        case G_ROTATE_270: // 270度
            lcd_write_data(0xE8);
            break;
        default:
            lcd_write_data(0x08); // 默认0度
    }
	delay(100);
	LCD_WIDTH = w;
	LCD_HEIGHT = h;
	if(_lcd_buffer != NULL) {
		free(_lcd_buffer);
	}
	if(_shadow_argb != NULL) {
		free(_shadow_argb);
	}
	_lcd_buffer = malloc(LCD_WIDTH * LCD_HEIGHT * 2);
	_shadow_argb = malloc(LCD_WIDTH * LCD_HEIGHT * sizeof(uint32_t));
	_shadow_ready = 0;
	_pending_flush = 0;
	_last_flush_ms = 0;
	if(_shadow_argb != NULL) {
		memset(_shadow_argb, 0xff, LCD_WIDTH * LCD_HEIGHT * sizeof(uint32_t));
	}
	LCD_FLUSH_MODE = LCD_FLUSH_PARTIAL;
}

static inline void lcd_brightness(uint8_t brightness) {
	// chyba trzeba wczesniej zainicjalizowac - rejestr 0x53
	lcd_write_command(0x51); // byc moze 2 bajty?
	lcd_write_data(brightness); // byc moze 2 bajty
}

static inline void lcd_set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
	uint16_t x1 = x + w - 1;
	uint16_t y1 = y + h - 1;

	if(ILI9486_REG_WIDTH_16) {
		uint8_t buf[8];

		/* Column Address Set - command as bulk */
		buf[0] = 0x00; buf[1] = 0x2A;
		bsp_gpio_write(LCD_DC, 0);
		bsp_spi_send_recv(buf, NULL, 2);
		bsp_gpio_write(LCD_DC, 1);
		/* parameters as single bulk */
		buf[0] = 0x00; buf[1] = (x >> 8) & 0xff;
		buf[2] = 0x00; buf[3] = x & 0xff;
		buf[4] = 0x00; buf[5] = (x1 >> 8) & 0xff;
		buf[6] = 0x00; buf[7] = x1 & 0xff;
		bsp_spi_send_recv(buf, NULL, 8);

		/* Page Address Set */
		buf[0] = 0x00; buf[1] = 0x2B;
		bsp_gpio_write(LCD_DC, 0);
		bsp_spi_send_recv(buf, NULL, 2);
		bsp_gpio_write(LCD_DC, 1);
		buf[0] = 0x00; buf[1] = (y >> 8) & 0xff;
		buf[2] = 0x00; buf[3] = y & 0xff;
		buf[4] = 0x00; buf[5] = (y1 >> 8) & 0xff;
		buf[6] = 0x00; buf[7] = y1 & 0xff;
		bsp_spi_send_recv(buf, NULL, 8);

		/* Memory Write */
		buf[0] = 0x00; buf[1] = 0x2C;
		bsp_gpio_write(LCD_DC, 0);
		bsp_spi_send_recv(buf, NULL, 2);
		bsp_gpio_write(LCD_DC, 1);
		return;
	}

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

	lcd_write_command(0x2C);
}

static inline void lcd_show(void) {
	lcd_set_window(0, 0, LCD_WIDTH, LCD_HEIGHT);
	bsp_spi_send_recv((const uint8_t*)_lcd_buffer, NULL,
			(uint32_t)LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t));
}

static inline void lcd_show_rows(uint16_t y, uint16_t h) {
	uint16_t* row_ptr;
	uint32_t row_bytes;

	if(h == 0) {
		return;
	}

	lcd_set_window(0, y, LCD_WIDTH, h);
	row_ptr = _lcd_buffer + ((uint32_t)y * LCD_WIDTH);
	row_bytes = (uint32_t)LCD_WIDTH * h * sizeof(uint16_t);
	bsp_spi_send_recv((const uint8_t*)row_ptr, NULL, row_bytes);
}

static uint16_t* _rect_buf = NULL;
static uint32_t _rect_buf_sz = 0;

static inline void lcd_show_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
	uint16_t row;
	uint32_t rect_bytes;
	if(w == 0 || h == 0)
		return;

	if(w == LCD_WIDTH) {
		/* full-width: use contiguous transfer */
		lcd_show_rows(y, h);
		return;
	}

	rect_bytes = (uint32_t)w * h * sizeof(uint16_t);

	/* ensure temp buffer is large enough */
	if(_rect_buf_sz < rect_bytes) {
		if(_rect_buf) free(_rect_buf);
		_rect_buf = malloc(rect_bytes);
		_rect_buf_sz = _rect_buf ? rect_bytes : 0;
	}

	if(_rect_buf != NULL) {
		/* copy rect pixels to contiguous buffer */
		for(row = 0; row < h; row++) {
			memcpy(_rect_buf + (uint32_t)row * w,
					_lcd_buffer + (uint32_t)(y + row) * LCD_WIDTH + x,
					(uint32_t)w * sizeof(uint16_t));
		}
		lcd_set_window(x, y, w, h);
		bsp_spi_send_recv((const uint8_t*)_rect_buf, NULL, rect_bytes);
	} else {
		/* fallback: per-row if malloc failed */
		lcd_set_window(x, y, w, h);
		for(row = y; row < y + h; row++) {
			bsp_spi_send_recv((const uint8_t*)(_lcd_buffer + (uint32_t)row * LCD_WIDTH + x),
					NULL, (uint32_t)w * sizeof(uint16_t));
		}
	}
}

static inline uint16_t argb_to_rgb565_be(uint32_t pixel) {
	uint16_t r = (pixel >> 19) & 0x1f;
	uint16_t g = (pixel >> 10) & 0x3f;
	uint16_t b = (pixel >> 3) & 0x1f;
	uint16_t rgb565 = (r << 11) | (g << 5) | b;
	return (rgb565 >> 8) | (rgb565 << 8);
}

void ili9486_flush(const void* buf, uint32_t size) {
	if(size < LCD_WIDTH * LCD_HEIGHT* 4)
		return;
	if(_lcd_buffer == NULL)
		return;

	uint32_t *src = (uint32_t*)buf;
	uint32_t sz = LCD_HEIGHT * LCD_WIDTH;
	uint32_t x;
	uint32_t y;
	uint32_t idx;
	uint32_t min_x, max_x, min_y, max_y;
	uint32_t dirty_w, dirty_h, dirty_area, total_area;
	uint8_t dirty;
	uint64_t now_ms;

	if(_shadow_argb == NULL || !_shadow_ready) {
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
		_pending_flush = 0;
		_last_flush_ms = kernel_tic_ms(0);
		return;
	}

	min_x = LCD_WIDTH;
	max_x = 0;
	min_y = LCD_HEIGHT;
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
				if(x < min_x) min_x = x;
				if(x > max_x) max_x = x;
				if(y < min_y) min_y = y;
				if(y > max_y) max_y = y;
			}
		}
	}

	if(!dirty) {
		/* no change this frame, flush deferred dirty rect if pending */
		if(_pending_flush) {
			now_ms = kernel_tic_ms(0);
			if(_last_flush_ms == 0 ||
					(now_ms - _last_flush_ms) >= FLUSH_MIN_INTERVAL_MS) {
				bsp_spi_set_div(SPI_DIV);
				lcd_start();
				lcd_show_rect(_pend_min_x, _pend_min_y,
						_pend_max_x - _pend_min_x + 1,
						_pend_max_y - _pend_min_y + 1);
				lcd_end();
				_pending_flush = 0;
				_last_flush_ms = now_ms;
			}
		}
		return;
	}

	dirty_w = max_x - min_x + 1;
	dirty_h = max_y - min_y + 1;
	dirty_area = dirty_w * dirty_h;
	total_area = (uint32_t)LCD_WIDTH * LCD_HEIGHT;
	now_ms = kernel_tic_ms(0);

	bsp_spi_set_div(SPI_DIV);
	if(LCD_FLUSH_MODE == LCD_FLUSH_FULL) {
		lcd_start();
		lcd_show();
		lcd_end();
		_pending_flush = 0;
		_last_flush_ms = now_ms;
	}
	else if(_last_flush_ms != 0 &&
			(now_ms - _last_flush_ms) < FLUSH_MIN_INTERVAL_MS &&
			dirty_area <= total_area / 8) {
		/* small rapid change: defer and accumulate dirty rect */
		if(!_pending_flush) {
			_pend_min_x = min_x;
			_pend_max_x = max_x;
			_pend_min_y = min_y;
			_pend_max_y = max_y;
		} else {
			if(min_x < _pend_min_x) _pend_min_x = min_x;
			if(max_x > _pend_max_x) _pend_max_x = max_x;
			if(min_y < _pend_min_y) _pend_min_y = min_y;
			if(max_y > _pend_max_y) _pend_max_y = max_y;
		}
		_pending_flush = 1;
	}
	else if(dirty_area > total_area / DIRTY_AREA_FULL_THRESHOLD) {
		/* large dirty area: full refresh more efficient */
		lcd_start();
		lcd_show();
		lcd_end();
		_pending_flush = 0;
		_last_flush_ms = now_ms;
	}
	else {
		/* partial refresh: send only dirty rect */
		if(_pending_flush) {
			/* merge accumulated pending rect */
			if(_pend_min_x < min_x) min_x = _pend_min_x;
			if(_pend_max_x > max_x) max_x = _pend_max_x;
			if(_pend_min_y < min_y) min_y = _pend_min_y;
			if(_pend_max_y > max_y) max_y = _pend_max_y;
			dirty_w = max_x - min_x + 1;
			dirty_h = max_y - min_y + 1;
		}
		lcd_start();
		lcd_show_rect(min_x, min_y, dirty_w, dirty_h);
		lcd_end();
		_pending_flush = 0;
		_last_flush_ms = now_ms;
	}
}

void ili9486_set_flush_mode(uint16_t mode) {
	LCD_FLUSH_MODE = mode;
	_pending_flush = 0;
}

void ili9486_init(uint16_t w, uint16_t h, uint16_t rot, uint16_t inversion,
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
	lcd_write_command(0x01); // sw reset, wakeup
	delay(150000);

	lcd_write_command(0x28); // Display OFF
	lcd_write_command(0x3A); // Interface Pixel Format
	lcd_write_data(0x55);	// 16 bit/pixel

	lcd_write_command(0xC2);
	lcd_write_data(0x44);    // 保持原配置
	lcd_write_command(0xC5);
	lcd_write_data(0x3E);    // VCOMH = 4.0V
	lcd_write_data(0x30);    // VCOML = -1.5V  
	lcd_write_data(0x0B);    // VCOM偏移
	lcd_write_data(0x0B);    // VCM输出

	// 设置亮度 - 0x00(最暗) 到 0xFF(最亮)
	lcd_write_command(0x51);    // Write Display Brightness
	lcd_write_data(0xf0);       // 50% 亮度 (128/255)

 	lcd_write_command(0xB7);   // 进入扩展命令集
    lcd_write_data(0x06);     // 启用抗闪烁模式

	/*lcd_write_command(0xC2); // Power Control 3 (For Normal Mode)
	lcd_write_data(0x44);    // Cos z napieciem

	lcd_write_command(0xC5); // VCOM Control
	lcd_write_data(0x00);  // const
	lcd_write_data(0x00);  // nVM ? 0x48
	lcd_write_data(0x00);  // VCOM voltage ref
	lcd_write_data(0x00);  // VCM out

	lcd_write_command(0xE0); // PGAMCTRL(Positive Gamma Control)
	lcd_write_data(0x0F);
	lcd_write_data(0x1F);
	lcd_write_data(0x1C);
	lcd_write_data(0x0C);
	lcd_write_data(0x0F);
	lcd_write_data(0x08);
	lcd_write_data(0x48);
	lcd_write_data(0x98);
	lcd_write_data(0x37);
	lcd_write_data(0x0A);
	lcd_write_data(0x13);
	lcd_write_data(0x04);
	lcd_write_data(0x11);
	lcd_write_data(0x0D);
	lcd_write_data(0x00);

	lcd_write_command(0xE1); // NGAMCTRL (Negative Gamma Correction)
	lcd_write_data(0x0F);
	lcd_write_data(0x32);
	lcd_write_data(0x2E);
	lcd_write_data(0x0B);
	lcd_write_data(0x0D);
	lcd_write_data(0x05);
	lcd_write_data(0x47);
	lcd_write_data(0x75);
	lcd_write_data(0x37);
	lcd_write_data(0x06);
	lcd_write_data(0x10);
	lcd_write_data(0x03);
	lcd_write_data(0x24);
	lcd_write_data(0x20);
	lcd_write_data(0x00);
	*/

	lcd_write_command(0x11); // sw reset, wakeup
	delay(150000);

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
