#include "uc_panel.h"
#include "uc_time.h"

#include <arch/bcm283x/gpio.h>

/*
 * Reset sequence matches panel-cwu50.c::cwu50_prepare():
 *   reset-gpio low → msleep(10) → high → msleep(120)
 * DT decl `reset-gpio = <&gpio 8 1>` (active low), so the "asserted"
 * level is 0 and the "deasserted" level (which releases the panel from
 * reset) is 1.
 */
void uc_panel_reset(void) {
	bcm283x_gpio_init();

	/*
	 * GPIO 8 defaults to SPI0_CE0 alt-func on CM4; force it back to a
	 * plain output driven by us.
	 */
	bcm283x_gpio_pull(UC_PANEL_RESET_GPIO, GPIO_PULL_NONE);
	bcm283x_gpio_config(UC_PANEL_RESET_GPIO, GPIO_OUTPUT);

	bcm283x_gpio_write(UC_PANEL_RESET_GPIO, 0);
	uc_mdelay(10);
	bcm283x_gpio_write(UC_PANEL_RESET_GPIO, 1);
	uc_mdelay(120);
}
