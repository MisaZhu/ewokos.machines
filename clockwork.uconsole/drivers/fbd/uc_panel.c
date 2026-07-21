#include "uc_panel.h"
#include "uc_time.h"

#include <arch/bcm283x/gpio.h>

/*
 * The DT overlay declares `reset-gpio = <&gpio 8 1>` — flag 1 is
 * GPIO_ACTIVE_LOW, so every gpiod logical value in panel-cwu50.c is
 * INVERTED on the physical pin:
 *
 *   probe:   devm_gpiod_get_optional(..., GPIOD_OUT_HIGH)
 *            → logical 1 → physical LOW
 *   prepare: gpiod_set_value(reset, 0)  → physical HIGH, msleep(10)
 *            gpiod_set_value(reset, 1)  → physical LOW,  msleep(120)
 *            ... DCS init follows, pin stays physical LOW while running.
 *
 * Getting this backwards holds the panel in reset for ever: backlight
 * (GPIO 9) still lights, but no DCS command is ever accepted.
 */

/* probe-time state: pin claimed as output, driven physical LOW. */
void uc_panel_probe(void) {
	bcm283x_gpio_init();

	/*
	 * GPIO 8 defaults to SPI0_CE0 alt-func on CM4; force it back to a
	 * plain output driven by us.
	 */
	bcm283x_gpio_pull(UC_PANEL_RESET_GPIO, GPIO_PULL_NONE);
	bcm283x_gpio_config(UC_PANEL_RESET_GPIO, GPIO_OUTPUT);
	bcm283x_gpio_write(UC_PANEL_RESET_GPIO, 0);
}

/*
 * prepare-time reset pulse. Must run with the DSI host already up in
 * LP-11 (i.e. after uc_dsi_bringup()), immediately before the DCS init
 * table — the same point in the sequence where DRM calls
 * panel->prepare().
 */
void uc_panel_reset(void) {
	uc_panel_probe();

	bcm283x_gpio_write(UC_PANEL_RESET_GPIO, 1);   /* gpiod 0 → phys HIGH */
	uc_mdelay(10);
	bcm283x_gpio_write(UC_PANEL_RESET_GPIO, 0);   /* gpiod 1 → phys LOW  */
	uc_mdelay(120);
}

/*
 * Brute-force polarity probe: the exact opposite waveform. The normal
 * sequence is derived from the decompiled DT overlay + gpiod ACTIVE_LOW
 * inversion; if the real wiring differs from that model (inverter on
 * the flex, different panel batch), the normal sequence holds the panel
 * in reset for ever and it stays deaf to every command -- exactly the
 * observed symptom. This variant pulses physical LOW for 10ms and then
 * parks the pin physical HIGH while commands run.
 */
void uc_panel_reset_inverted(void) {
	uc_panel_probe();

	bcm283x_gpio_write(UC_PANEL_RESET_GPIO, 0);
	uc_mdelay(10);
	bcm283x_gpio_write(UC_PANEL_RESET_GPIO, 1);
	uc_mdelay(120);
}
