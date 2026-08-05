#ifndef BCM2712_GPIO_H
#define BCM2712_GPIO_H

#include <stdint.h>
#include <stdbool.h>

/*
 * RP1 GPIO driver for BCM2712 (Raspberry Pi 5).
 *
 * The RP1 south bridge provides 54 GPIO pins:
 *   Bank 0: GPIO 0-27
 *   Bank 1: GPIO 28-41
 *   Bank 2: GPIO 42-53
 *
 * Each bank is at a 0x4000 stride within the RP1 GPIO region.
 * RP1 GPIO base (RP1-internal): 0x400d0000
 * Host offset from _mmio_base: 0x080d0000
 */

/* RP1 GPIO offset from _mmio_base */
#define RP1_GPIO_BASE_OFF  0x080d0000
#define RP1_GPIO_BANK_STRIDE 0x4000
#define RP1_NUM_GPIOS       54

/* Per-bank register offsets */
#define GPIO_OUT_SET     0x18
#define GPIO_OUT_CLR     0x1c
#define GPIO_OUT         0x20
#define GPIO_IN          0x24
#define GPIO_OE_SET      0x28
#define GPIO_OE_CLR      0x2c
#define GPIO_OE          0x30

/* Function select registers (0x00-0x14, 5 bits per pin, 6 pins per reg) */
#define GPIO_FSEL0       0x00
#define GPIO_FSEL1       0x04
#define GPIO_FSEL2       0x08
#define GPIO_FSEL3       0x0c
#define GPIO_FSEL4       0x10

/* Pull control (per-bank, offset 0x100) */
#define GPIO_PULLUP      0x100
#define GPIO_PULLDOWN    0x104

/* Function select values */
#define GPIO_FUNC_ALTF0  0
#define GPIO_FUNC_ALTF1  1
#define GPIO_FUNC_ALTF2  2
#define GPIO_FUNC_ALTF3  3
#define GPIO_FUNC_ALTF4  4
#define GPIO_FUNC_OUTPUT 5
#define GPIO_FUNC_ALTF6  6
#define GPIO_FUNC_ALTF7  7
#define GPIO_FUNC_ALTF8  8
#define GPIO_FUNC_ALTF9  9

/* Pull resistor values */
#define GPIO_PULL_NONE   0
#define GPIO_PULL_UP     1
#define GPIO_PULL_DOWN   2

void bcm2712_gpio_init(void);
void bcm2712_gpio_config(uint32_t pin, uint32_t func);
void bcm2712_gpio_pull(uint32_t pin, uint32_t pull);
void bcm2712_gpio_write(uint32_t pin, bool high);
bool bcm2712_gpio_read(uint32_t pin);

#endif
