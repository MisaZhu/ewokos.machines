#include "uc_fb.h"

#include <arch/bcm283x/framebuffer.h>

int32_t uc_fb_mailbox_init(uint32_t w, uint32_t h, uint32_t dep) {
	return bcm283x_fb_init(w, h, dep);
}

fbinfo_t* uc_fb_mailbox_info(void) {
	return bcm283x_get_fbinfo();
}
