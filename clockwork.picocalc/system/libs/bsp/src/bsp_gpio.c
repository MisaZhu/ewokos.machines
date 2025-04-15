#include "gpio/gpio.h"
#include "bsp/bsp_gpio.h"
#include <arch/rk3506/gpio.h>

#ifdef __cplusplus
extern "C" {
#endif

void bsp_bsp_gpio_init(void) {
    rk_gpio_init();
}

void bsp_gpio_config(int32_t gpio_no, int32_t gpio_sel) {
    rk_gpio_config(gpio_no, gpio_sel);
}

void bsp_gpio_pull(int32_t gpio_no, int32_t pull_dir) {
    rk_gpio_pull(gpio_no, pull_dir);
}

void bsp_gpio_write(int32_t gpio_no, int32_t value) {
    rk_gpio_write(gpio_no, value); 
}

uint8_t  bsp_gpio_read(int32_t gpio_no) {
    return rk_gpio_read(gpio_no);
}

#ifdef __cplusplus
}
#endif
