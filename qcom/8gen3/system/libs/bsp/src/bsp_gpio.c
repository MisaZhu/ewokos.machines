#include <bsp/bsp_gpio.h>

/* Q8B user-space TLMM access is not implemented yet. */
void bsp_gpio_init(void) {
}

void bsp_gpio_config(int32_t gpio_no, int32_t gpio_sel) {
    (void)gpio_no;
    (void)gpio_sel;
}

void bsp_gpio_pull(int32_t gpio_no, int32_t pull_dir) {
    (void)gpio_no;
    (void)pull_dir;
}

void bsp_gpio_write(int32_t gpio_no, int32_t value) {
    (void)gpio_no;
    (void)value;
}

uint8_t bsp_gpio_read(int32_t gpio_no) {
    (void)gpio_no;
    return 0;
}
