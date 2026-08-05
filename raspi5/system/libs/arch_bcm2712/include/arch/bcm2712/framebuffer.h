#ifndef BCM2712_FRAMEBUFFER_H
#define BCM2712_FRAMEBUFFER_H

#include <stdint.h>

/*
 * Framebuffer interface using VPU mailbox property tags.
 * Allocates a framebuffer through the GPU and returns its parameters.
 */

typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t depth;       /* bits per pixel */
	uint32_t pitch;       /* bytes per row */
	uint32_t size;        /* total framebuffer size in bytes */
	void     *buf;        /* pointer to framebuffer (bus address) */
} bcm2712_fb_info_t;

int  bcm2712_fb_init(uint32_t width, uint32_t height, uint32_t depth);
void bcm2712_get_fbinfo(bcm2712_fb_info_t *info);

#endif
