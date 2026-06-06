#include <arch/bcm283x/dsi.h>
#include <arch/bcm283x/framebuffer.h>
#include <arch/bcm283x/gpio.h>

#include <stddef.h>
#include <unistd.h>

static int _gpio_ready = 0;

static void dsi_sleep_ms(uint32_t ms) {
	if (ms == 0) {
		return;
	}
	usleep(ms * 1000);
}

static void dsi_gpio_prepare(void) {
	if (_gpio_ready != 0) {
		return;
	}
	bcm283x_gpio_init();
	_gpio_ready = 1;
}

static int dsi_gpio_level(int active_high, int asserted) {
	if (active_high != 0) {
		return asserted ? 1 : 0;
	}
	return asserted ? 0 : 1;
}

void bcm283x_dsi_panel_init_default(bcm283x_dsi_panel_t* panel) {
	if (panel == NULL) {
		return;
	}

	panel->reset_gpio = -1;
	panel->reset_active_high = 0;
	panel->reset_hold_ms = 20;
	panel->reset_settle_ms = 120;

	panel->backlight_gpio = -1;
	panel->backlight_active_high = 1;
	panel->backlight_delay_ms = 0;

	panel->preferred_width = 800;
	panel->preferred_height = 480;
	panel->preferred_depth = 32;
}

int bcm283x_dsi_panel_prepare(const bcm283x_dsi_panel_t* panel) {
	if (panel == NULL || panel->reset_gpio < 0) {
		return 0;
	}

	dsi_gpio_prepare();
	bcm283x_gpio_config(panel->reset_gpio, GPIO_OUTPUT);

	bcm283x_gpio_write(panel->reset_gpio,
			dsi_gpio_level(panel->reset_active_high, 0));
	dsi_sleep_ms(1);
	bcm283x_gpio_write(panel->reset_gpio,
			dsi_gpio_level(panel->reset_active_high, 1));
	dsi_sleep_ms(panel->reset_hold_ms);
	bcm283x_gpio_write(panel->reset_gpio,
			dsi_gpio_level(panel->reset_active_high, 0));
	dsi_sleep_ms(panel->reset_settle_ms);
	return 0;
}

int bcm283x_dsi_panel_enable_backlight(const bcm283x_dsi_panel_t* panel) {
	if (panel == NULL || panel->backlight_gpio < 0) {
		return 0;
	}

	dsi_gpio_prepare();
	bcm283x_gpio_config(panel->backlight_gpio, GPIO_OUTPUT);
	bcm283x_gpio_write(panel->backlight_gpio,
			dsi_gpio_level(panel->backlight_active_high, 1));
	dsi_sleep_ms(panel->backlight_delay_ms);
	return 0;
}

int32_t bcm283x_dsi_fb_init(const bcm283x_dsi_panel_t* panel,
		uint32_t w, uint32_t h, uint32_t dep) {
	bcm283x_dsi_panel_t fallback_panel;

	if (panel == NULL) {
		bcm283x_dsi_panel_init_default(&fallback_panel);
		panel = &fallback_panel;
	}

	if (w == 0) {
		w = panel->preferred_width;
	}
	if (h == 0) {
		h = panel->preferred_height;
	}
	if (dep == 0) {
		dep = panel->preferred_depth;
	}

	if (bcm283x_dsi_panel_prepare(panel) != 0) {
		return -1;
	}
	if (bcm283x_fb_init(w, h, dep) != 0) {
		return -1;
	}
	return bcm283x_dsi_panel_enable_backlight(panel);
}

fbinfo_t* bcm283x_dsi_get_fbinfo(void) {
	return bcm283x_get_fbinfo();
}
