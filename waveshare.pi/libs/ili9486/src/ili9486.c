#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ewoksys/vdevice.h>
#include <bsp/bsp_gpio.h>
#include <bsp/bsp_spi.h>
#include <ili9486/ili9486.h>

static int LCD_DC =	24;
static int LCD_CS	= 8;
static int LCD_RST = 25;
static int SPI_DIV = 2;

// Screen settings
#define DEF_SCREEN_WIDTH 		320
#define DEF_SCREEN_HEIGHT 		240

// Other define
#define SCREEN_VERTICAL_1		0
#define SCREEN_HORIZONTAL_1		1
#define SCREEN_VERTICAL_2		2
#define SCREEN_HORIZONTAL_2		3

static uint16_t* _lcd_buffer = NULL;
uint16_t LCD_WIDTH  = DEF_SCREEN_WIDTH;
uint16_t LCD_HEIGHT = DEF_SCREEN_HEIGHT;

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
static inline void lcd_set_buffer(uint16_t w, uint16_t h) {
	lcd_write_command(0x36);
	lcd_write_data(0x28);
	delay(100);
	LCD_WIDTH = w;
	LCD_HEIGHT = h;
	_lcd_buffer = malloc(LCD_WIDTH * LCD_HEIGHT * 2);
}

static inline void lcd_brightness(uint8_t brightness) {
	// chyba trzeba wczesniej zainicjalizowac - rejestr 0x53
	lcd_write_command(0x51); // byc moze 2 bajty?
	lcd_write_data(brightness); // byc moze 2 bajty
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

	lcd_write_command(0x2C); // Memory write?
}

static inline void lcd_show(void) {
	int i, j, m = 0;
	lcd_set_size(LCD_WIDTH, LCD_HEIGHT);

#define SPI_FIFO_SIZE  64
	uint8_t c8[SPI_FIFO_SIZE];

	for ( i = 0 ; i < (LCD_WIDTH*4/64) ; i ++ ) {
		uint16_t *tx_data = (uint16_t*)&_lcd_buffer[16*LCD_HEIGHT*i];
		int32_t data_sz = 16*LCD_HEIGHT;
		for( j=0; j<data_sz; j++)  {
			uint16_t color = tx_data[j];
			c8[m++] = (color >> 8) & 0xff;
			c8[m++] = (color) & 0xff;
			if(m >= SPI_FIFO_SIZE) {
				m = 0;
				bsp_spi_send_recv(c8, NULL, SPI_FIFO_SIZE);
			}
		}
	}
}

void ili9486_flush(const void* buf, uint32_t size) {
	if(size < LCD_WIDTH * LCD_HEIGHT* 4)
		return;

	uint32_t *src = (uint32_t*)buf;
	uint32_t sz = LCD_HEIGHT*LCD_WIDTH;
	uint32_t i;

	for (i = 0; i < sz; i++) {
		register uint32_t s = src[i];
		register uint8_t r = (s >> 16) & 0xff;
		register uint8_t g = (s >> 8)  & 0xff;
		register uint8_t b = s & 0xff;
		_lcd_buffer[i] = ((r >> 3) <<11) | ((g >> 2) << 5) | (b >> 3);
	}

	bsp_spi_set_div(SPI_DIV);
	lcd_start();
	lcd_show();
	lcd_end();
}

void ili9486_init(uint16_t w, uint16_t h, int pin_rs, int pin_cs, int pin_rst, int cdiv) {
	LCD_DC = pin_rs;
	LCD_CS = pin_cs;
	LCD_RST = pin_rst;
	bsp_gpio_init();
	bsp_gpio_config(LCD_DC, GPIO_OUTPUT);
	bsp_gpio_config(LCD_CS, GPIO_OUTPUT);
	bsp_gpio_config(LCD_RST, GPIO_OUTPUT);

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

	lcd_write_command(0x20); // Display Inversion OFF   RPi LCD (A)
	//lcd_write_command(0x21); // Display Inversion ON    RPi LCD (B)

	//lcd_write_command(0x36); // Memory Access Control
	//lcd_write_data(0x48);
	lcd_set_buffer(w, h);
	lcd_write_command(0x29); // Display ON
	delay(150000);
	lcd_end();
}