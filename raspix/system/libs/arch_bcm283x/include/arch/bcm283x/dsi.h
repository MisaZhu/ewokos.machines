#ifndef BCM283X_DSI_H
#define BCM283X_DSI_H

#include <stdint.h>
#include <ewoksys/fbinfo.h>

typedef struct {
	int32_t reset_gpio;
	int32_t reset_active_high;
	uint32_t reset_hold_ms;
	uint32_t reset_settle_ms;

	int32_t backlight_gpio;
	int32_t backlight_active_high;
	uint32_t backlight_delay_ms;

	uint32_t preferred_width;
	uint32_t preferred_height;
	uint32_t preferred_depth;
} bcm283x_dsi_panel_t;

void bcm283x_dsi_panel_init_default(bcm283x_dsi_panel_t* panel);
int bcm283x_dsi_panel_prepare(const bcm283x_dsi_panel_t* panel);
int bcm283x_dsi_panel_enable_backlight(const bcm283x_dsi_panel_t* panel);
int32_t bcm283x_dsi_fb_init(const bcm283x_dsi_panel_t* panel,
		uint32_t w, uint32_t h, uint32_t dep);
fbinfo_t* bcm283x_dsi_get_fbinfo(void);

#endif
