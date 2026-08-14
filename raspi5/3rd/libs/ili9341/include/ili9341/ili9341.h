#ifndef ILI9341_H
#define ILI9341_H

#include <stdint.h>
#include <ewoksys/fbinfo.h>
#include <graph/graph_ex.h>

extern uint16_t LCD_HEIGHT;
extern uint16_t LCD_WIDTH;

void ili9341_set_config(int pin_dc, int pin_cs, int pin_rst, int pin_bl,
    int cdiv, int spi_select);
void ili9341_init(uint16_t w, uint16_t h, uint16_t rot, uint16_t inversion,
    int pin_dc, int pin_cs, int pin_rst, int pin_bl, int cdiv);
void ili9341_flush(const void* buf, uint32_t size);
uint32_t ili9341_flush_rect(const fbinfo_t* fbinfo, const graph_t* g, const grect_t* r);

#endif
