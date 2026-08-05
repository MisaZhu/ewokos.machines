#ifndef BSP_GPIO_H
#define BSP_GPIO_H

#include <stdint.h>
#include <arch/bcm2712/gpio.h>

void     bsp_gpio_init(void);
void     bsp_gpio_config(int32_t gpio_no, int32_t gpio_sel);
void     bsp_gpio_pull(int32_t gpio_no, int32_t pull_dir);
void     bsp_gpio_write(int32_t gpio_no, int32_t value);
uint8_t  bsp_gpio_read(int32_t gpio_no);

#endif
