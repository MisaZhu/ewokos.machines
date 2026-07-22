#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>

#include "spi.h"
#include "lcd.h"

#define LCD_RST 2
#define LCD_DC	3
#define LCD_CS	4

static int  hres = 320;
static int    vres = 320;

static void sleep_ms(int ms){
	while(ms--)_delay(10000);
}

static void GPIO_SET(int pin, int val){
	//if(pin == LCD_CS && val == 1)
	//_delay(100);
	rk_gpio_write(pin, val);
	//if(pin == LCD_CS && val == 0)
	//_delay(100);
}

static void spi_write_data(uint8_t data){
    GPIO_SET(LCD_DC, 1);
    GPIO_SET(LCD_CS, 0);
    rk_spi_write(&data, 1);
    GPIO_SET(LCD_CS, 1);
}

static uint8_t  spi_read_data(uint8_t reg){
	uint8_t ret;
	GPIO_SET(LCD_DC, 0);
    GPIO_SET(LCD_CS, 0);
	rk_spi_write(&reg, 1);
	rk_spi_read(&ret, 1);
    GPIO_SET(LCD_CS, 1);
	return ret;
}

static void spi_write_data24(uint32_t data){

}

static void spi_write_command(uint8_t data){
    GPIO_SET(LCD_DC, 0);
    GPIO_SET(LCD_CS, 0);
    rk_spi_write(&data, 1);
    GPIO_SET(LCD_CS, 1);
}

static void spi_write_cd(uint8_t command, int len, ...) {
    int i;
    va_list ap;
    va_start(ap, len);
    spi_write_command(command);
    for (i = 0; i < len; i++) spi_write_data((char) va_arg(ap, int));
    va_end(ap);
}

void define_region_spi(int xstart, int ystart, int xend, int yend, int rw) {
    unsigned char coord[4];
	spi_write_command(0x2A); // Column addr set
	spi_write_data(xstart >> 8);
	spi_write_data(xstart & 0xFF);     // XSTART
	spi_write_data(xend >> 8);
	spi_write_data(xend & 0xFF);     // XEND

	spi_write_command(0x2B); // Row addr set
	spi_write_data(ystart>>8);
	spi_write_data(ystart &0xff);     // YSTART
	spi_write_data(yend>>8);
	spi_write_data(yend &0xff);     // YEND
	spi_write_command(0x2C);
						   // G
}

void draw_buffer_spi(int x1, int y1, int x2, int y2, unsigned char *p) {
    int i, t;
    unsigned char q[3];

    int pixelCount = (x2 - x1 + 1) * (y2 - y1 + 1);
    uint16_t *pixelBuffer = (uint16_t *)p;

    define_region_spi(x1, y1, x2, y2, 1);

    GPIO_SET(LCD_DC, 1);
    GPIO_SET(LCD_CS, 0);
    for (i = 0; i < pixelCount; i++) {
//        uint16_t pixel = pixelBuffer[i];
//
//        // Extract RGB565 components
//        uint8_t r5 = (pixel >> 11) & 0x1F;
//        uint8_t g6 = (pixel >> 5) & 0x3F;
//        uint8_t b5 = pixel & 0x1F;
//
//        // Convert to 8-bit values (scaling approximation)
//        uint8_t r8 = (r5 << 3) | (r5 >> 2);
//        uint8_t g8 = (g6 << 2) | (g6 >> 4);
//        uint8_t b8 = (b5 << 3) | (b5 >> 2);
//
        // Convert each RGB565 pixel to RGB888 (3 bytes per pixel) for ILI9488
        uint8_t rgb[3];
        rgb[0] = 0xff;  // Red
        rgb[1] = 0x00;  // Green
        rgb[2] = 0x80;  // Blue
        rk_spi_write(rgb, 3);
    }

	GPIO_SET(LCD_CS, 1);
}

void lcd_clean(int x1, int y1, int x2, int y2, uint32_t color) {
    int i, t;
    int pixelCount = (x2 - x1 + 1) * (y2 - y1 + 1);
    define_region_spi(x1, y1, x2, y2, 1);
    GPIO_SET(LCD_DC, 1);
    GPIO_SET(LCD_CS, 0);
    for (i = 0; i < pixelCount; i++) {
           rk_spi_write((uint8_t*)color, 3);
    }

	GPIO_SET(LCD_CS, 1);
}

void lcd_init(void){
	rk_gpio_init();
	rk_spi_init();
	rk_gpio_config(LCD_RST , 1);
	rk_gpio_config(LCD_DC,	1);
	rk_gpio_config(LCD_CS, 1);

	GPIO_SET(LCD_CS, 1);
	GPIO_SET(LCD_RST, 0);
	_delay(100000000);
	GPIO_SET(LCD_RST, 1);
	_delay(100000000);

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

	lcd_clean(0, 0, 319, 319, 0xFFFF0000);
}
