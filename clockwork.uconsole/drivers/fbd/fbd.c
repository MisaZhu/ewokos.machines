#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>

#include <arch/bcm283x/dsi.h>
#include <fbd/fbd.h>
#include <graph/graph.h>

#define UCONSOLE_NATIVE_WIDTH   720U
#define UCONSOLE_NATIVE_HEIGHT  1280U
static const char* _conf_file = "";
static bcm283x_dsi_panel_t _panel;

static void uconsole_panel_init(void) {
	bcm283x_dsi_panel_init_default(&_panel);

	/*
	 * Public ClockworkPi CM4 patches describe the uConsole panel as a DSI
	 * panel using GPIO8 for reset and GPIO9 for backlight control.
	 */
	_panel.reset_gpio = 8;
	_panel.reset_active_high = 0;
	_panel.reset_hold_ms = 20;
	_panel.reset_settle_ms = 120;

	_panel.backlight_gpio = 9;
	_panel.backlight_active_high = 1;
	_panel.backlight_delay_ms = 0;

	_panel.preferred_width = UCONSOLE_NATIVE_WIDTH;
	_panel.preferred_height = UCONSOLE_NATIVE_HEIGHT;
	_panel.preferred_depth = 32;
}

static uint16_t rgb565_from_u32(uint32_t s) {
	uint8_t r = (uint8_t)((s >> 16) & 0xff);
	uint8_t g = (uint8_t)((s >> 8) & 0xff);
	uint8_t b = (uint8_t)(s & 0xff);
	return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static uint32_t blt32_pitch(const fbinfo_t* fbinfo, const graph_t* g) {
	uint32_t bytes_per_pixel = fbinfo->depth / 8;
	uint8_t* dst = (uint8_t*)(uintptr_t)(fbinfo->pointer +
			fbinfo->yoffset * fbinfo->pitch +
			fbinfo->xoffset * bytes_per_pixel);
	const uint32_t* src = g->buffer;
	uint32_t row_bytes = (uint32_t)g->w * bytes_per_pixel;
	uint32_t total_bytes = (uint32_t)g->h * row_bytes;

	if (fbinfo->pitch == row_bytes) {
		memcpy(dst, src, total_bytes);
		return total_bytes;
	}

	for (int32_t y = 0; y < g->h; ++y) {
		uint8_t* dst_row = dst + y * fbinfo->pitch;
		const uint8_t* src_row = (const uint8_t*)(src + y * g->w);
		memcpy(dst_row, src_row, row_bytes);
	}
	return total_bytes;
}

static uint32_t blt16_pitch(const fbinfo_t* fbinfo, const graph_t* g) {
	uint8_t* dst_base = (uint8_t*)(uintptr_t)fbinfo->pointer +
			fbinfo->yoffset * fbinfo->pitch +
			fbinfo->xoffset * 2;

	for (int32_t y = 0; y < g->h; ++y) {
		uint16_t* dst_row = (uint16_t*)(dst_base + y * fbinfo->pitch);
		const uint32_t* src_row = g->buffer + y * g->w;

		for (int32_t x = 0; x < g->w; ++x) {
			dst_row[x] = rgb565_from_u32(src_row[x]);
		}
	}
	return (uint32_t)g->w * (uint32_t)g->h * 2U;
}

static uint32_t flush(const fbinfo_t* fbinfo, const graph_t* g) {
	const fbinfo_t* phy = fbinfo != NULL ? fbinfo : bcm283x_dsi_get_fbinfo();
	if (phy == NULL || phy->pointer == 0) {
		return 0;
	}

	if (phy->depth != 32 && phy->depth != 16) {
		return 0;
	}

	if (phy->depth == 16) {
		return blt16_pitch(phy, g);
	}
	return blt32_pitch(phy, g);
}

static fbinfo_t* get_info(void) {
	return bcm283x_dsi_get_fbinfo();
}

static int32_t init(uint32_t w, uint32_t h, uint32_t dep) {
	(void)w;
	(void)h;

	if (dep == 0) {
		dep = _panel.preferred_depth;
	}

	int32_t ret = bcm283x_dsi_fb_init(&_panel,
			UCONSOLE_NATIVE_WIDTH, UCONSOLE_NATIVE_HEIGHT, dep);
	return ret;
}

static int doargs(int argc, char* argv[]) {
	int c = 0;

	while (c != -1) {
		c = getopt(argc, argv, "c:");
		if (c == -1) {
			break;
		}

		switch (c) {
		case 'c':
			_conf_file = optarg;
			break;
		default:
			c = -1;
			break;
		}
	}
	return optind;
}

int main(int argc, char** argv) {
	fbd_t fbd;
	uconsole_panel_init();

	int opti = doargs(argc, argv);
	const char* mnt_point = (opti < argc && opti >= 0) ? argv[opti] : "/dev/fb0";

	fbd.splash = NULL;
	fbd.flush = flush;
	fbd.init = init;
	fbd.get_info = get_info;

	return fbd_run(&fbd, mnt_point,
			UCONSOLE_NATIVE_WIDTH, UCONSOLE_NATIVE_HEIGHT, _conf_file);
}
