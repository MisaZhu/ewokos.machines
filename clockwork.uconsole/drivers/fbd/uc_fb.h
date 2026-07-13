#ifndef UC_FB_H
#define UC_FB_H

#include <stdint.h>
#include <ewoksys/fbinfo.h>

/*
 * Fallback framebuffer path: ask the VideoCore mailbox to allocate/
 * configure a framebuffer, exactly the same way the vanilla raspix fbd
 * does. On the uConsole this drives whatever HDMI/DPI output is enabled
 * on the CM4 baseboard (or nothing at all if the panel is the only
 * display), which is useful for early bring-up before the native DSI
 * path in Phase 3/4 works.
 *
 * uc_fb_mailbox_init() is a thin wrapper over bcm283x_fb_init() from the
 * SDK.  On success uc_fb_mailbox_info() returns the fbinfo negotiated
 * with VideoCore.
 */

int32_t   uc_fb_mailbox_init(uint32_t w, uint32_t h, uint32_t dep);
fbinfo_t* uc_fb_mailbox_info(void);

#endif
