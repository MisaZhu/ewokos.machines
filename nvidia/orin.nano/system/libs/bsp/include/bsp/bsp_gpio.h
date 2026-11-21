#ifndef bsp_gpio_BSP_H
#define bsp_gpio_BSP_H

#include <stdint.h>

void     bsp_gpio_init(void);
void     bsp_gpio_config(int32_t bsp_gpio_no, int32_t bsp_gpio_sel);
void     bsp_gpio_pull(int32_t bsp_gpio_no, int32_t pull_dir);
void     bsp_gpio_write(int32_t bsp_gpio_no, int32_t value);
uint8_t  bsp_gpio_read(int32_t bsp_gpio_no);

/* Read a main pinmux register (offset within pinmux@2430000).  Fields:
 * [1:0] function, [3:2] pull, bit4 tristate, bit6 input-en, bit10 GPIO/SFIO
 * (1=GPIO).  Returns 0xffffffff on map failure. */
uint32_t bsp_pinmux_read(uint32_t offset);

/* gpiod service (/dev/gpio0) dev_cntl commands. */
#define ORIN_GPIO_CMD_READ    0x47500001
#define ORIN_GPIO_CMD_WRITE   0x47500002
#define ORIN_GPIO_CMD_CONFIG  0x47500003
#define ORIN_GPIO_CMD_PINMUX  0x47500004

#endif
