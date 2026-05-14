#include <bsp/bsp_fb.h>

static fbinfo_t _fbinfo;

fbinfo_t* bsp_get_fbinfo(void) {
	return &_fbinfo;
}

int32_t bsp_fb_init(uint32_t w, uint32_t h, uint32_t dep) {
	_fbinfo.pointer = 0;
	_fbinfo.size = 0;
	_fbinfo.size_max = 0;
	_fbinfo.width = w;
	_fbinfo.height = h;
	_fbinfo.vwidth = w;
	_fbinfo.vheight = h;
	_fbinfo.depth = dep;
	_fbinfo.pitch = w * (dep / 8);
	_fbinfo.xoffset = 0;
	_fbinfo.yoffset = 0;
	_fbinfo.dma_id = -1;
	_fbinfo.phy_base = 0;
	return -1;
}
