#ifndef BCM2712_FRAMEBUFFER_H
#define BCM2712_FRAMEBUFFER_H

#include <ewoksys/fbinfo.h>

fbinfo_t* bcm2712_get_fbinfo(void);
int32_t bcm2712_fb_init(uint32_t w, uint32_t h, uint32_t dep);

#endif
