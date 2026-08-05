#include <string.h>
#include <stdio.h>
#include <ewoksys/dma.h>
#include <arch/bcm2712/mailbox.h>
#include <arch/bcm2712/framebuffer.h>

/*
 * VPU mailbox framebuffer allocation (simpler than RP1 TCON).
 * Uses legacy mailbox property tags to request a framebuffer
 * from the GPU firmware.
 */
#define MBOX_TAG_SET_PHYS_WH   0x00048003
#define MBOX_TAG_SET_VIRT_WH   0x00048004
#define MBOX_TAG_SET_DEPTH     0x00048005
#define MBOX_TAG_ALLOCATE_FB   0x00040001
#define MBOX_TAG_GET_PITCH     0x00040008

#define MAILBOX_VC_ALIAS_NONCACHED 0x40000000u

static bcm2712_fb_info_t _fb_info;

int bcm2712_fb_init(uint32_t width, uint32_t height, uint32_t depth) {
	(void)width;
	(void)height;
	(void)depth;

	/* Stub: VPU mailbox framebuffer not yet implemented.
	 * For initial bring-up, return success with zeroed info. */
	memset(&_fb_info, 0, sizeof(_fb_info));
	return -1;
}

void bcm2712_get_fbinfo(bcm2712_fb_info_t *info) {
	if (info)
		memcpy(info, &_fb_info, sizeof(*info));
}
