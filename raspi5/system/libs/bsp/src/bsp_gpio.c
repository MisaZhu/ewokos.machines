#include "bsp/bsp_gpio.h"
#include <arch/bcm2712/gpio.h>

void bsp_gpio_init(void) {
	bcm2712_gpio_init();
}

void bsp_gpio_config(int32_t gpio_no, int32_t gpio_sel) {
	bcm2712_gpio_config(gpio_no, gpio_sel);
}

void bsp_gpio_pull(int32_t gpio_no, int32_t pull_dir) {
	bcm2712_gpio_pull(gpio_no, pull_dir);
}

void bsp_gpio_write(int32_t gpio_no, int32_t value) {
	bcm2712_gpio_write(gpio_no, value);
}

uint8_t bsp_gpio_read(int32_t gpio_no) {
	return bcm2712_gpio_read(gpio_no);
}
