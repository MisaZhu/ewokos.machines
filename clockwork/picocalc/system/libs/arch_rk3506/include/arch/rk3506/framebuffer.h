#ifndef RK3506_FRAMEBUFFER_H
#define RK3506_FRAMEBUFFER_H

#include <ewoksys/dispinfo.h>

disp_info_t* rk3506_get_fbinfo(void);
int32_t rk3506_fb_init(uint32_t w, uint32_t h, uint32_t dep);

#endif
