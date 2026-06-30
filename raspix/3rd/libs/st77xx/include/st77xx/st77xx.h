#ifndef st77xx_H
#define st77xx_H

#include <stdint.h>

#define G_ROTATE_0 0
#define G_ROTATE_90 1
#define G_ROTATE_180 2
#define G_ROTATE_270 3

extern uint16_t LCD_HEIGHT;
extern uint16_t LCD_WIDTH;
extern uint16_t LCD_MODE;
extern uint16_t LCD_FLUSH_MODE;

enum {
  LCD_MODE_0 = 0x00,
  LCD_MODE_1,
  LCD_MODE_2
};

enum {
  LCD_FLUSH_AUTO = 0,
  LCD_FLUSH_FULL
};

/*LCD_DC	Instruction/Data Register selection
  LCD_CS    LCD chip selection, low active
  LCD_RST   LCD reset
  */
void st77xx_init(uint16_t w, uint16_t h, uint16_t rot, uint16_t inversion,
    int pin_rs, int pin_dc, int pin_rst, int pin_bl, int cdiv);
void st77xx_flush(const void* buf, uint32_t size);
void st77xx_set_view(uint16_t w, uint16_t h, uint16_t rot, uint16_t mode);
void st77xx_set_flush_mode(uint16_t mode);

#endif
