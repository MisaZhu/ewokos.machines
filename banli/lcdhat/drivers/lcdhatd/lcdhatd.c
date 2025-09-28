#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <arch/bcm283x/gpio.h>
#include <arch/bcm283x/spi.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/vfs.h>
#include <ewoksys/syscall.h>
#include "banli_1inch54.h"

#define UBYTE   uint8_t
#define UWORD   uint16_t
#define UDOUBLE uint32_t

#define LCD_CS   24
#define LCD_RST  27
#define LCD_DC   25
#define LCD_BL   18

#define DEV_Delay_ms(x) proc_usleep((x)*1000)
#define DEV_Digital_Write bcm283x_gpio_write

#define LCD_CS_0		DEV_Digital_Write(LCD_CS,0)
#define LCD_CS_1		DEV_Digital_Write(LCD_CS,1)

#define LCD_RST_0		DEV_Digital_Write(LCD_RST,0)
#define LCD_RST_1		DEV_Digital_Write(LCD_RST,1)

#define LCD_DC_0		DEV_Digital_Write(LCD_DC,0)
#define LCD_DC_1		DEV_Digital_Write(LCD_DC,1)

#define LCD_BL_0		DEV_Digital_Write(LCD_BL,0)
#define LCD_BL_1		DEV_Digital_Write(LCD_BL,1)

#define ROT_0        0
#define ROT_90       1
#define ROT_180      2
#define ROT_270      3

typedef struct{
	UWORD WIDTH;
	UWORD HEIGHT;
	UBYTE ROT;
}LCD_ATTRIBUTES;
static LCD_ATTRIBUTES LCD;

static inline void DEV_SPI_Write(UBYTE* data, uint32_t sz) {
	bcm283x_spi_activate(1);
	for(uint32_t i=0; i<sz; i++)
		bcm283x_spi_transfer(data[i]);
	bcm283x_spi_activate(0);
}

/******************************************************************************
function :	Hardware reset
parameter:
 ******************************************************************************/
static void LCD_Reset(void) {
	LCD_RST_1;
	DEV_Delay_ms(100);
	LCD_RST_0;
	DEV_Delay_ms(100);
	LCD_RST_1;
	DEV_Delay_ms(100);
}

/******************************************************************************
function :	send command
parameter:
Reg : Command register
 ******************************************************************************/
static void LCD_SendCommand(UBYTE Reg) {
	LCD_DC_0;
	// LCD_CS_0;
	DEV_SPI_Write(&Reg, 1);
	// LCD_CS_1;
}

/******************************************************************************
function :	send data
parameter:
    Data : Write data
 ******************************************************************************/
static void LCD_SendData_8Bit(UBYTE Data) {
	LCD_DC_1;
	// LCD_CS_0;
	DEV_SPI_Write(&Data, 1);
	// LCD_CS_1;
}

/******************************************************************************
function :	send data
parameter:
    Data : Write data
 ******************************************************************************/
/*static void LCD_SendData_16Bit(UWORD Data) {
	LCD_DC_1;
	// LCD_CS_0;
	DEV_SPI_WriteByte((Data >> 8) & 0xFF);
	DEV_SPI_WriteByte(Data & 0xFF);
	// LCD_CS_1;
}
*/

/********************************************************************************
function:	Set the resolution and scanning method of the screen
parameter:
		rot:   Scan direction
 ********************************************************************************/
static void LCD_SetAttributes(UBYTE rot, uint32_t w, uint32_t h) {
	//Get the screen scan direction
	LCD.ROT = rot;
	UBYTE MemoryAccessReg = 0x00;

	//Get GRAM and LCD width and height
	// 斑梨电子1.54寸LCD固定分辨率
	LCD.HEIGHT	= BANLI_LCD_HEIGHT;
	LCD.WIDTH   = BANLI_LCD_WIDTH;
	MemoryAccessReg = 0X70;

	// Set the read / write scan direction of the frame memory
	LCD_SendCommand(0x36); //MX, MY, RGB mode
	LCD_SendData_8Bit(MemoryAccessReg);	//0x08 set RGB
}

/******************************************************************************
function :	Initialize the lcd register
parameter:
 ******************************************************************************/
static void LCD_InitReg(void) {
	// 斑梨电子1.54寸LCD专用初始化序列
	LCD_SendCommand(ST7789_SWRESET);
	usleep(120 * 1000);
	
	LCD_SendCommand(ST7789_SLPOUT);
	usleep(120 * 1000);
	
	LCD_SendCommand(ST7789_COLMOD);
	LCD_SendData_8Bit(0x55);  // RGB565格式
	
	LCD_SendCommand(ST7789_MADCTL);
	LCD_SendData_8Bit(0x00);  // 内存访问控制
	
	LCD_SendCommand(ST7789_INVOFF);
	
	LCD_SendCommand(ST7789_NORON);
	usleep(10 * 1000);
	
	LCD_SendCommand(ST7789_DISPON);
	usleep(120 * 1000);
}

/********************************************************************************
function :	Initialize the lcd
parameter:
 ********************************************************************************/
static void LCD_1in3_Init(UBYTE rot, uint32_t w, uint32_t h) {
	//Turn on the backlight
	LCD_BL_1;

	//Hardware reset
	LCD_Reset();
	//Set the resolution and scanning method of the screen
	LCD_SetAttributes(rot, w, h);

	//Set the initialization register
	LCD_InitReg();
}

/********************************************************************************
function:	Sets the start position and size of the displaylay area
parameter:
		Xstart 	:   X direction Start coordinates
		Ystart  :   Y direction Start coordinates
		Xend    :   X direction end coordinates
Yend    :   Y direction end coordinates
 ********************************************************************************/
static void LCD_1in3_SetWindows(UWORD Xstart, UWORD Ystart, UWORD Xend, UWORD Yend) {
	//set the X coordinates
	LCD_SendCommand(0x2A);
	LCD_SendData_8Bit((Xstart >> 8) & 0xFF);
	LCD_SendData_8Bit(Xstart & 0xFF);
	LCD_SendData_8Bit(((Xend  - 1) >> 8) & 0xFF);
	LCD_SendData_8Bit((Xend  - 1) & 0xFF);

	//set the Y coordinates
	LCD_SendCommand(0x2B);
	LCD_SendData_8Bit((Ystart >> 8) & 0xFF);
	LCD_SendData_8Bit(Ystart & 0xFF);
	LCD_SendData_8Bit(((Yend  - 1) >> 8) & 0xFF);
	LCD_SendData_8Bit((Yend  - 1) & 0xFF);

	LCD_SendCommand(0X2C);
}

/******************************************************************************
function :	Clear screen
parameter:
 ******************************************************************************/
static void LCD_1in3_Clear(UWORD Color) {
	UWORD j;
	UWORD Image[LCD.WIDTH*LCD.HEIGHT];

	Color = ((Color<<8)&0xff00)|(Color>>8);

	for (j = 0; j < LCD.HEIGHT*LCD.WIDTH; j++) {
		Image[j] = Color;
	}

	//LCD_1in3_SetWindows(0, 0, LCD.WIDTH, LCD.HEIGHT);
	LCD_DC_1;
	DEV_SPI_Write((uint8_t *)Image, LCD.WIDTH*LCD.HEIGHT*2);
}

void lcd_init(uint32_t w, uint32_t h, uint32_t rot, uint32_t div) {
	bcm283x_gpio_init();

	bcm283x_gpio_config(LCD_CS, 1);
	bcm283x_gpio_config(LCD_RST, 1);
	bcm283x_gpio_config(LCD_DC, 1);
	bcm283x_gpio_config(LCD_BL, 1);

	bcm283x_spi_init();
	bcm283x_spi_set_div(div);
	bcm283x_spi_select(1);

	LCD_1in3_Init(rot, w, h);
	LCD_1in3_SetWindows(0, 0, LCD.WIDTH, LCD.HEIGHT);
	LCD_1in3_Clear(0x0);
}

static inline uint16_t  rgb32_rgb565(uint8_t *src){
	return (src[2] & 0xF8) | (src[1] >> 5) | ((src[1] &0x1C) << 11)  | ((src[0] & 0xF8) << 5);
}

int  do_flush(const void* buf, uint32_t size) {
	if(size < LCD.WIDTH * LCD.HEIGHT* 4)
		return -1;

	LCD_DC_1;
	bcm283x_spi_activate(1);

	uint32_t *src = (uint32_t*)buf;
	uint32_t sz = LCD.HEIGHT*LCD.WIDTH;
	uint32_t i, j = 0;

#define SPI_FIFO_SIZE  64
	uint16_t c8[SPI_FIFO_SIZE/2];

	if(LCD.ROT == ROT_0 || LCD.ROT == ROT_90) {
		for (i = 0; i <sz; i++) {
			c8[j++] = rgb32_rgb565((uint8_t*)&src[i]);
			if(j >= SPI_FIFO_SIZE/2) {
				j = 0;
				bcm283x_spi_send_recv((uint8_t*)c8, NULL, SPI_FIFO_SIZE);
			}
		}
	}
	else {
		for (i = sz; i > 0; i--) {
			c8[j++] = rgb32_rgb565((uint8_t*)&src[i - 1]);
			if(j >= SPI_FIFO_SIZE/2) {
				j = 0;
				bcm283x_spi_send_recv((uint8_t*)c8, NULL, SPI_FIFO_SIZE);
			}
		}
	}
	bcm283x_spi_activate(0);
	return 0;
}

