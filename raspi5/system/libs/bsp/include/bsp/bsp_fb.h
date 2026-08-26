#ifndef BSP_FRAMEBUFFER_H
#define BSP_FRAMEBUFFER_H

#include <ewoksys/dispinfo.h>
#include <arch/bcm2712/rp1_dpi.h>

disp_info_t* bsp_get_fbinfo(void);
int32_t bsp_fb_init(uint32_t w, uint32_t h, uint32_t dep);
int32_t bsp_fb_init_dpi(uint32_t w, uint32_t h, uint32_t dep,
		const bcm2712_dpi_timing_t *timing);

#endif
