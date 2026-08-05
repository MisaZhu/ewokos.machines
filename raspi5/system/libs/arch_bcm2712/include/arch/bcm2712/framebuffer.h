#ifndef BCM2712_FRAMEBUFFER_H
#define BCM2712_FRAMEBUFFER_H

#include <ewoksys/fbinfo.h>

/* firmware pixel order (mailbox tag 0x00040006) */
#define PIXEL_ORDER_BGR 0u
#define PIXEL_ORDER_RGB 1u

/*
 * firmware alpha mode (mailbox tag 0x00040007)
 * 0: alpha enabled  (0 = fully opaque)
 * 1: alpha reversed (0 = fully transparent)
 * 2: alpha ignored
 */
#define ALPHA_MODE_ENABLED  0u
#define ALPHA_MODE_REVERSED 1u
#define ALPHA_MODE_IGNORED  2u

fbinfo_t* bcm2712_get_fbinfo(void);
int32_t bcm2712_fb_init(uint32_t w, uint32_t h, uint32_t dep);

/*
 * bcm2712_fb_init() asks the firmware for ARGB8888 (depth 32, pixel order BGR,
 * alpha ignored) and then reads back what it actually got. These report the
 * result, so the blitter knows whether any software fixup is still needed.
 * On a firmware that honoured the request both return 0 and the flush path is
 * a plain copy.
 */
uint32_t bcm2712_fb_pixel_order(void);
uint32_t bcm2712_fb_alpha_mode(void);

#endif
