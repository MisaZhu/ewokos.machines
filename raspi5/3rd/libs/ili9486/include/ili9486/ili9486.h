#ifndef ILI9486_H
#define ILI9486_H

#include <stdint.h>

#define G_ROTATE_0 0
#define G_ROTATE_90 1
#define G_ROTATE_180 2
#define G_ROTATE_270 3

#define LCD_FLUSH_PARTIAL 0
#define LCD_FLUSH_FULL 1

#define ILI9486_INIT_PROFILE_GENERIC 0
#define ILI9486_INIT_PROFILE_WS35C 1

extern uint16_t LCD_HEIGHT;
extern uint16_t LCD_WIDTH;
extern int ILI9486_REG_WIDTH_16;
extern int ILI9486_INIT_PROFILE;

/*LCD_DC	Instruction/Data Register selection
  LCD_CS    LCD chip selection, low active
  LCD_RST   LCD reset
  */
void ili9486_set_config(int pin_dc, int pin_cs, int pin_rst, int pin_bl,
    int cdiv, int spi_select);
void ili9486_init(uint16_t w, uint16_t h, uint16_t rot, uint16_t inversion,
    int pin_dc, int pin_cs, int pin_rst, int pin_bl, int cdiv);
void ili9486_flush(const void* buf, uint32_t size);
void ili9486_set_flush_mode(uint16_t mode);

#endif
