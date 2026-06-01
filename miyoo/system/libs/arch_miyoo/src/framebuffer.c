#include <string.h>
#include <arch/miyoo/framebuffer.h>
#include <ewoksys/syscall.h>
#include <sysinfo.h>
#include <ewoksys/mmio.h>

static fbinfo_t _fb_info;

#define MIYOO_FB_PHY_BASE 0x27c00000U
#define MIYOO_FB_PLUS_OFFSET 628U
#define MIYOO_FB_RESERVED_SIZE (4U * 1024U * 1024U)

typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t vwidth;
	uint32_t vheight;
	uint32_t bytes;
	uint32_t depth;
	uint32_t ignorex;
	uint32_t ignorey;
	void * pointer;
	uint32_t size;
} fb_init_t;

int32_t miyoo_fb_init(uint32_t w, uint32_t h, uint32_t dep) {
	(void)w;
	(void)h;
	(void)dep;
	ewokos_addr_t phy_base = MIYOO_FB_PHY_BASE;
	uint32_t reserved_size = MIYOO_FB_RESERVED_SIZE;

	_fb_info.width = 640;
	_fb_info.height = 480;
	_fb_info.vwidth = 640;
	_fb_info.vheight = 480;
	_fb_info.depth = 16;
	_fb_info.pitch = _fb_info.width*(_fb_info.depth/8);

	sys_info_t sysinfo;
	syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&sysinfo);

	if(strstr(sysinfo.machine, "miyoo-plus") != NULL) {
		phy_base += MIYOO_FB_PLUS_OFFSET; // GPU addr to ARM addr
		reserved_size -= MIYOO_FB_PLUS_OFFSET;
	}

	_fb_info.pointer = (void*)syscall1(SYS_P2V, phy_base);
	if(_fb_info.pointer == NULL)
		return -1;

	_fb_info.size = _fb_info.width*_fb_info.height*2;
	_fb_info.size_max = reserved_size;
	_fb_info.xoffset = 0;
	_fb_info.yoffset = 0;

	if(syscall3(SYS_MEM_MAP, (ewokos_addr_t)_fb_info.pointer, phy_base, (ewokos_addr_t)_fb_info.size_max) == 0)
		return -1;
	return 0;
}

fbinfo_t* miyoo_get_fbinfo(void) {
	return &_fb_info;
}
