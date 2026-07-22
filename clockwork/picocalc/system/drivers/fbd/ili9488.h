#ifndef _ILI9488_H_
#define _ILI9488_H_

#include <stdint.h>

#define LCD_WIDTH 320
#define LCD_HEIGHT 320

#define PIXFMT_BGR 1

#define TFT_SLPOUT 0x11
#define TFT_INVOFF 0x20
#define TFT_INVON 0x21

#define TFT_DISPOFF 0x28
#define TFT_DISPON 0x29
#define TFT_MADCTL 0x36

#define ILI9341_MEMCONTROL 	0x36
#define ILI9341_MADCTL_MX  	0x40
#define ILI9341_MADCTL_BGR 	0x08

#define ILI9341_COLADDRSET      0x2A
#define ILI9341_PAGEADDRSET     0x2B
#define ILI9341_MEMORYWRITE     0x2C
#define ILI9341_RAMRD           0x2E

#define ILI9341_Portrait        ILI9341_MADCTL_MX | ILI9341_MADCTL_BGR

#define ORIENT_NORMAL       0

#define RGB(red, green, blue) (unsigned int) (((red & 0b11111111) << 16) | ((green  & 0b11111111) << 8) | (blue & 0b11111111))
#define WHITE               RGB(255,  255,  255) //0b1111
#define YELLOW              RGB(255,  255,    0) //0b1110
#define LILAC               RGB(255,  128,  255) //0b1101
#define BROWN               RGB(255,  128,    0) //0b1100
#define FUCHSIA             RGB(255,  64,   255) //0b1011
#define RUST                RGB(255,  64,     0) //0b1010
#define MAGENTA             RGB(255,  0,    255) //0b1001
#define RED                 RGB(255,  0,      0) //0b1000
#define CYAN                RGB(0,    255,  255) //0b0111
#define GREEN               RGB(0,    255,    0) //0b0110
#define CERULEAN            RGB(0,    128,  255) //0b0101
#define MIDGREEN            RGB(0,    128,    0) //0b0100
#define COBALT              RGB(0,    64,   255) //0b0011
#define MYRTLE              RGB(0,    64,     0) //0b0010
#define BLUE                RGB(0,    0,    255) //0b0001
#define BLACK               RGB(0,    0,      0) //0b0000
#define BROWN               RGB(255,  128,    0)
#define GRAY                RGB(128,  128,    128)
#define LITEGRAY            RGB(210,  210,    210)
#define ORANGE            	RGB(0xff,	0xA5,	0)
#define PINK				RGB(0xFF,	0xA0,	0xAB)
#define GOLD				RGB(0xFF,	0xD7,	0x00)
#define SALMON				RGB(0xFA,	0x80,	0x72)
#define BEIGE				RGB(0xF5,	0xF5,	0xDC)


#define PORTCLR             1
#define PORTSET             2
#define PORTINV             3
#define LAT                 4
#define LATCLR              5
#define LATSET              6
#define LATINV              7
#define ODC                 8
#define ODCCLR              9
#define ODCSET              10
#define CNPU                12
#define CNPUCLR             13
#define CNPUSET             14
#define CNPUINV             15
#define CNPD                16
#define CNPDCLR             17
#define CNPDSET             18

#define ANSELCLR            -7
#define ANSELSET            -6
#define ANSELINV            -5
#define TRIS                -4
#define TRISCLR             -3
#define TRISSET             -2

void ili9488_init(void);
void ili9488_clear(uint32_t argb);
void ili9488_draw(int x, int y, int w, int h, uint32_t *argb);

#endif
