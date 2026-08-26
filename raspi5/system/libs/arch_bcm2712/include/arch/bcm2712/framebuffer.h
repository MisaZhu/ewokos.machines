#ifndef BCM2712_FRAMEBUFFER_H
#define BCM2712_FRAMEBUFFER_H

#include <ewoksys/dispinfo.h>
#include <arch/bcm2712/rp1_dpi.h>

disp_info_t* bcm2712_get_fbinfo(void);
int32_t bcm2712_fb_init(uint32_t w, uint32_t h, uint32_t dep);
/* RP1 DPI (parallel display) output; independent of the HDMI path above */
int32_t bcm2712_fb_init_dpi(uint32_t w, uint32_t h, uint32_t dep,
		const bcm2712_dpi_timing_t *timing);

#endif
