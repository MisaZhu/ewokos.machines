#ifndef BCM2712_NATIVE_HDMI_H
#define BCM2712_NATIVE_HDMI_H

#include <stdint.h>
#include <ewoksys/fbinfo.h>
#include <sysinfo.h>

int bcm2712_native_hdmi_supported(uint32_t w, uint32_t h, uint32_t dep);
int bcm2712_native_hdmi_init(const sys_info_t *sysinfo,
		uint32_t w, uint32_t h, uint32_t dep,
		fbinfo_t *info);

#endif
