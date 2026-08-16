#include <bsp/bsp_fb.h>
#include <stdint.h>
#include <string.h>
#include <arch/bcm2712/framebuffer.h>

fbinfo_t* bsp_get_fbinfo(void) {
    return bcm2712_get_fbinfo();
}

int32_t bsp_fb_init(uint32_t w, uint32_t h, uint32_t dep) {
    return bcm2712_fb_init(w, h, dep);
}

int32_t bsp_fb_init_dpi(uint32_t w, uint32_t h, uint32_t dep,
        const bcm2712_dpi_timing_t *timing) {
    return bcm2712_fb_init_dpi(w, h, dep, timing);
}
