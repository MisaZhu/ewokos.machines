#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <graph/graph.h>
#include <ewoksys/vdevice.h>
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
	//Get the screen scan direction
	lcd_write_command(0x36); //MX, MY, RGB mode
	switch(rot) {
        case G_ROTATE_NONE: // 0度
            lcd_write_data(0x48);
            break;
        case G_ROTATE_90: // 90度
            lcd_write_data(0x28);
            break;
        case G_ROTATE_180: // 180度
            lcd_write_data(0x88);
            break;
        case G_ROTATE_270: // 270度
            lcd_write_data(0xf8);
            break;
        default:
            lcd_write_data(0x48); // 默认0度
    }
	delay(100);
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

void st77xx_flush(const void* buf, uint32_t size) {
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