#include <arch/bcm2712/gpio.h>
#include <ewoksys/mmio.h>

/*
 * RP1 GPIO bank layout:
 *   Bank 0 (base + 0x0000): GPIO 0-27  (28 pins)
 *   Bank 1 (base + 0x4000): GPIO 28-41 (14 pins)
 *   Bank 2 (base + 0x8000): GPIO 42-53 (12 pins)
 */

static uint32_t _gpio_base;

static inline uint32_t gpio_bank(uint32_t pin) {
	if (pin < 28) return 0;
	if (pin < 42) return 1;
	return 2;
}

static inline uint32_t gpio_bank_pin(uint32_t pin) {
	if (pin < 28) return pin;
	if (pin < 42) return pin - 28;
	return pin - 42;
}

static inline uint32_t gpio_bank_base(uint32_t pin) {
	return _gpio_base + gpio_bank(pin) * RP1_GPIO_BANK_STRIDE;
}

void bcm2712_gpio_init(void) {
	_gpio_base = _mmio_base + RP1_GPIO_BASE_OFF;
}

void bcm2712_gpio_config(uint32_t pin, uint32_t func) {
	if (pin >= RP1_NUM_GPIOS) return;

	uint32_t bank_base = gpio_bank_base(pin);
	uint32_t bp = gpio_bank_pin(pin);
	uint32_t fsel_reg = GPIO_FSEL0 + (bp / 6) * 4;
	uint32_t shift = (bp % 6) * 5;

	uint32_t val = get32(bank_base + fsel_reg);
	val &= ~(0x1f << shift);
	val |= (func & 0x1f) << shift;
	put32(bank_base + fsel_reg, val);
}

void bcm2712_gpio_pull(uint32_t pin, uint32_t pull) {
	if (pin >= RP1_NUM_GPIOS) return;

	uint32_t bank_base = gpio_bank_base(pin);
	uint32_t bp = gpio_bank_pin(pin);

	switch (pull) {
	case GPIO_PULL_UP:
		put32(bank_base + GPIO_PULLUP, 1 << bp);
		break;
	case GPIO_PULL_DOWN:
		put32(bank_base + GPIO_PULLDOWN, 1 << bp);
		break;
	default:
		/* Clear both pull-up and pull-down for NONE */
		put32(bank_base + GPIO_PULLUP, 1 << bp);
		put32(bank_base + GPIO_PULLDOWN, 1 << bp);
		break;
	}
}

void bcm2712_gpio_write(uint32_t pin, bool high) {
	if (pin >= RP1_NUM_GPIOS) return;

	uint32_t bank_base = gpio_bank_base(pin);
	uint32_t bp = gpio_bank_pin(pin);

	if (high)
		put32(bank_base + GPIO_OUT_SET, 1 << bp);
	else
		put32(bank_base + GPIO_OUT_CLR, 1 << bp);
}

bool bcm2712_gpio_read(uint32_t pin) {
	if (pin >= RP1_NUM_GPIOS) return false;

	uint32_t bank_base = gpio_bank_base(pin);
	uint32_t bp = gpio_bank_pin(pin);

	return (get32(bank_base + GPIO_IN) & (1 << bp)) != 0;
}
