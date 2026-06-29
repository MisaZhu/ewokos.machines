#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <graph/graph.h>
#include <ewoksys/vdevice.h>
#include <bsp/bsp_gpio.h>
#include <bsp/bsp_spi.h>
#include <ili9341/ili9341.h>

static int LCD_DC = 22;
static int LCD_CS = 8;
static int LCD_RST = 27;
static int LCD_BL = -1;
static int SPI_DIV = 16;

#define DEF_SCREEN_WIDTH  320
#define DEF_SCREEN_HEIGHT 240
#define LCD_MAX_PIXELS (DEF_SCREEN_WIDTH * DEF_SCREEN_HEIGHT)

static uint16_t _lcd_buffer[LCD_MAX_PIXELS];
uint16_t LCD_WIDTH = DEF_SCREEN_WIDTH;
uint16_t LCD_HEIGHT = DEF_SCREEN_HEIGHT;
static uint16_t _dbg_rot = 0;
static uint16_t _dbg_inversion = 0;

static inline void delay(int32_t count) {
	proc_usleep(count);
}

static inline void lcd_spi_send(uint8_t byte) {
	bsp_spi_transfer(byte);
}

static inline void lcd_write_command(uint8_t command) {
	bsp_gpio_write(LCD_DC, 0);
	lcd_spi_send(command);
	bsp_gpio_write(LCD_DC, 1);
}

static inline void lcd_write_data(uint8_t data) {
	lcd_spi_send(data);
}

static inline void lcd_reset(void) {
	if(LCD_BL >= 0) {
		bsp_gpio_write(LCD_BL, 1);
	}
	bsp_gpio_write(LCD_RST, 1);
	delay(100000);
	bsp_gpio_write(LCD_RST, 0);
	delay(100000);
	bsp_gpio_write(LCD_RST, 1);
	delay(120000);
}

static inline void lcd_start(void) {
	bsp_spi_select(SPI_SELECT_0);
	bsp_spi_activate(1);
}

static inline void lcd_end(void) {
	bsp_spi_activate(0);
}

static inline void lcd_set_buffer(uint16_t w, uint16_t h, uint16_t rot) {
	uint8_t mod = 0x48;

	switch(rot) {
	case G_ROTATE_90:
		mod = 0xE8;
		break;
	case G_ROTATE_180:
		mod = 0x88;
		break;
	case G_ROTATE_270:
		mod = 0x28;
		break;
	default:
		mod = 0x48;
		break;
	}

	lcd_write_command(0x36);
	lcd_write_data(mod);
	/* #region debug-point gnpe-ili9341-madctl-probe */
	klog("ili9341: set_buffer w=%u h=%u rot=%u madctl=%02x\n",
			(unsigned int)w, (unsigned int)h, (unsigned int)rot, mod);
	/* #endregion debug-point gnpe-ili9341-madctl-probe */

	LCD_WIDTH = w;
	LCD_HEIGHT = h;
}

static inline void lcd_set_size(uint16_t w, uint16_t h) {
	w--;
	h--;

	lcd_write_command(0x2A);
	lcd_write_data(0x00);
	lcd_write_data(0x00);
	lcd_write_data((w >> 8) & 0xff);
	lcd_write_data(w & 0xff);

	lcd_write_command(0x2B);
	lcd_write_data(0x00);
	lcd_write_data(0x00);
	lcd_write_data((h >> 8) & 0xff);
	lcd_write_data(h & 0xff);

	lcd_write_command(0x2C);
}

static inline void lcd_show(void) {
	uint32_t i;
	uint32_t sz = LCD_WIDTH * LCD_HEIGHT;
	uint32_t m = 0;
	lcd_set_size(LCD_WIDTH, LCD_HEIGHT);

#define SPI_FIFO_SIZE 64
	uint8_t c8[SPI_FIFO_SIZE];

	for(i = 0; i < sz; i++) {
		uint16_t color = _lcd_buffer[i];
		c8[m++] = (color >> 8) & 0xff;
		c8[m++] = color & 0xff;
		if(m >= SPI_FIFO_SIZE) {
			m = 0;
			bsp_spi_send_recv(c8, NULL, SPI_FIFO_SIZE);
		}
	}

	if(m > 0) {
		bsp_spi_send_recv(c8, NULL, m);
	}
}

void ili9341_flush(const void* buf, uint32_t size) {
	if(size < LCD_WIDTH * LCD_HEIGHT * 4) {
		/* #region debug-point gnpe-ili9341-madctl-probe */
		klog("ili9341: flush skipped size=%u expect=%u has_buf=%d\n",
				(unsigned int)size,
				(unsigned int)(LCD_WIDTH * LCD_HEIGHT * 4),
				1);
		/* #endregion debug-point gnpe-ili9341-madctl-probe */
		return;
	}

	uint32_t* src = (uint32_t*)buf;
	uint32_t sz = LCD_HEIGHT * LCD_WIDTH;
	uint32_t i;

	for(i = 0; i < sz; i++) {
		register uint32_t s = src[i];
		register uint8_t r = (s >> 16) & 0xff;
		register uint8_t g = (s >> 8) & 0xff;
		register uint8_t b = s & 0xff;
		_lcd_buffer[i] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
	}
	/* #region debug-point gnpe-ili9341-madctl-probe */
	if (sz > 0) {
		klog("ili9341: flush ok size=%u first_argb=%08x first_rgb565=%04x rot=%u inv=%u\n",
				(unsigned int)size,
				src[0],
				_lcd_buffer[0],
				(unsigned int)_dbg_rot,
				(unsigned int)_dbg_inversion);
	}
	/* #endregion debug-point gnpe-ili9341-madctl-probe */

	bsp_spi_set_div(SPI_DIV);
	lcd_start();
	lcd_show();
	lcd_end();
}

void ili9341_init(uint16_t w, uint16_t h, uint16_t rot, uint16_t inversion,
		int pin_dc, int pin_cs, int pin_rst, int pin_bl, int cdiv) {
	LCD_DC = pin_dc;
	LCD_CS = pin_cs;
	LCD_RST = pin_rst;
	LCD_BL = pin_bl;
	_dbg_rot = rot;
	_dbg_inversion = inversion;
	/* #region debug-point gnpe-ili9341-madctl-probe */
	klog("ili9341: init w=%u h=%u rot=%u inv=%u dc=%d cs=%d rst=%d bl=%d div=%d\n",
			(unsigned int)w, (unsigned int)h, (unsigned int)rot, (unsigned int)inversion,
			pin_dc, pin_cs, pin_rst, pin_bl, cdiv);
	/* #endregion debug-point gnpe-ili9341-madctl-probe */

	bsp_gpio_init();
	bsp_gpio_config(LCD_DC, GPIO_OUTPUT);
	bsp_gpio_config(LCD_RST, GPIO_OUTPUT);
	if(LCD_BL >= 0) {
		bsp_gpio_config(LCD_BL, GPIO_OUTPUT);
	}

	if(cdiv > 0) {
		SPI_DIV = cdiv;
	}
	bsp_spi_init();
	bsp_spi_set_div(SPI_DIV);
	bsp_spi_select(SPI_SELECT_0);
	bsp_gpio_write(LCD_DC, 1);
	lcd_reset();

	lcd_start();

	lcd_write_command(0x01);
	delay(150000);

	lcd_write_command(0x28);

	lcd_write_command(0xEF);
	lcd_write_data(0x03);
	lcd_write_data(0x80);
	lcd_write_data(0x02);

	lcd_write_command(0xCF);
	lcd_write_data(0x00);
	lcd_write_data(0xC1);
	lcd_write_data(0x30);

	lcd_write_command(0xED);
	lcd_write_data(0x64);
	lcd_write_data(0x03);
	lcd_write_data(0x12);
	lcd_write_data(0x81);

	lcd_write_command(0xE8);
	lcd_write_data(0x85);
	lcd_write_data(0x00);
	lcd_write_data(0x78);

	lcd_write_command(0xCB);
	lcd_write_data(0x39);
	lcd_write_data(0x2C);
	lcd_write_data(0x00);
	lcd_write_data(0x34);
	lcd_write_data(0x02);

	lcd_write_command(0xF7);
	lcd_write_data(0x20);

	lcd_write_command(0xEA);
	lcd_write_data(0x00);
	lcd_write_data(0x00);

	lcd_write_command(0xC0);
	lcd_write_data(0x23);

	lcd_write_command(0xC1);
	lcd_write_data(0x10);

	lcd_write_command(0xC5);
	lcd_write_data(0x3E);
	lcd_write_data(0x28);

	lcd_write_command(0xC7);
	lcd_write_data(0x86);

	lcd_write_command(0x3A);
	lcd_write_data(0x55);

	lcd_write_command(0xB1);
	lcd_write_data(0x00);
	lcd_write_data(0x18);

	lcd_write_command(0xB6);
	lcd_write_data(0x08);
	lcd_write_data(0x82);
	lcd_write_data(0x27);

	lcd_write_command(0xF2);
	lcd_write_data(0x00);

	lcd_write_command(0x26);
	lcd_write_data(0x01);

	lcd_write_command(0xE0);
	lcd_write_data(0x0F);
	lcd_write_data(0x31);
	lcd_write_data(0x2B);
	lcd_write_data(0x0C);
	lcd_write_data(0x0E);
	lcd_write_data(0x08);
	lcd_write_data(0x4E);
	lcd_write_data(0xF1);
	lcd_write_data(0x37);
	lcd_write_data(0x07);
	lcd_write_data(0x10);
	lcd_write_data(0x03);
	lcd_write_data(0x0E);
	lcd_write_data(0x09);
	lcd_write_data(0x00);

	lcd_write_command(0xE1);
	lcd_write_data(0x00);
	lcd_write_data(0x0E);
	lcd_write_data(0x14);
	lcd_write_data(0x03);
	lcd_write_data(0x11);
	lcd_write_data(0x07);
	lcd_write_data(0x31);
	lcd_write_data(0xC1);
	lcd_write_data(0x48);
	lcd_write_data(0x08);
	lcd_write_data(0x0F);
	lcd_write_data(0x0C);
	lcd_write_data(0x31);
	lcd_write_data(0x36);
	lcd_write_data(0x0F);

	lcd_write_command(0x11);
	delay(150000);

	lcd_set_buffer(w, h, rot);

	if(inversion == 0) {
		lcd_write_command(0x20);
	}
	else {
		lcd_write_command(0x21);
	}
	/* #region debug-point gnpe-ili9341-madctl-probe */
	klog("ili9341: display on rot=%u inv=%u width=%u height=%u\n",
			(unsigned int)rot, (unsigned int)inversion,
			(unsigned int)LCD_WIDTH, (unsigned int)LCD_HEIGHT);
	/* #endregion debug-point gnpe-ili9341-madctl-probe */

	lcd_write_command(0x29);
	delay(150000);
	lcd_end();
}
