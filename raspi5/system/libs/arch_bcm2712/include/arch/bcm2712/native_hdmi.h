#ifndef BCM2712_NATIVE_HDMI_H
#define BCM2712_NATIVE_HDMI_H

#include <stdint.h>
#include <ewoksys/fbinfo.h>
#include <sysinfo.h>

typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t depth;
	uint32_t pixel_clock_hz;
	uint32_t hfp;
	uint32_t hsync;
	uint32_t hbp;
	uint32_t vfp;
	uint32_t vsync;
	uint32_t vbp;
	uint8_t hsync_pos;
	uint8_t vsync_pos;
} bcm2712_hdmi_mode_t;

int bcm2712_native_hdmi_supported(uint32_t w, uint32_t h, uint32_t dep);
int bcm2712_native_hdmi_init_mode(const sys_info_t *sysinfo,
		const bcm2712_hdmi_mode_t *mode,
		fbinfo_t *info);
int bcm2712_native_hdmi_init(const sys_info_t *sysinfo,
		uint32_t w, uint32_t h, uint32_t dep,
		fbinfo_t *info);

#endif
