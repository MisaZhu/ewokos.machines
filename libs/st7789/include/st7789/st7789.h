#ifndef st7789_H
#define st7789_H

#include <stdint.h>

extern uint16_t LCD_HEIGHT;
extern uint16_t LCD_WIDTH;

/*LCD_DC	Instruction/Data Register selection
  LCD_CS    LCD chip selection, low active
  LCD_RST   LCD reset
  */
void st7789_init(uint16_t w, uint16_t h, uint16_t rot, uint16_t inversion,
    int pin_rs, int pin_dc, int pin_rst, int pin_bl, int cdiv);
void st7789_flush(const void* buf, uint32_t size);

#endif
