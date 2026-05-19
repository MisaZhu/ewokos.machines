#include <stdio.h>
#include <string.h>
#include <arch/bcm283x/mailbox.h>
#include <arch/bcm283x/framebuffer.h>
#include <ewoksys/syscall.h>
#include <sysinfo.h>
#include <ewoksys/mmio.h>
#include <ewoksys/dma.h>

static fbinfo_t _fb_info;

/*
 * QEMU's Raspberry Pi mailbox/property emulation is happiest with the
 * non-cached VC bus alias used elsewhere in raspix (board/sdhost/wlan).
 */
#define MAILBOX_VC_ALIAS_NONCACHED 0x40000000u
#define MAILBOX_VC_ALIAS_COHERENT 0xC0000000u
#define MAILBOX_RESPONSE_SUCCESS 0x80000000u
#define PIXEL_ORDER_RGB 1u

static uint32_t align_up(uint32_t value, uint32_t align) {
	return (value + align - 1) & (~(align - 1));
}

static int mailbox_call_with_alias(uint32_t* buffer, uint32_t alias, uint8_t channel) {
	mail_message_t msg;
	memset(&msg, 0, sizeof(mail_message_t));
	msg.data = (dma_phy_addr(0, (ewokos_addr_t)buffer) + alias) >> 4;
	msg.channel = channel;
	if (bcm283x_mailbox_call_timeout(&msg, 0) != 0) {
		return -1;
	}
	return (buffer[1] & MAILBOX_RESPONSE_SUCCESS) != 0 ? 0 : -1;
}

static int mailbox_property_call_with_fallback(uint32_t* buffer, uint32_t* alias_used) {
	uint32_t size = buffer[0];
	uint32_t* shadow = (uint32_t*)dma_alloc(0, size);

	if (shadow != NULL) {
		memcpy(shadow, buffer, size);
	}

	if (mailbox_call_with_alias(buffer, MAILBOX_VC_ALIAS_NONCACHED, PROPERTY_CHANNEL) == 0) {
		if (alias_used != NULL) {
			*alias_used = MAILBOX_VC_ALIAS_NONCACHED;
		}
		if (shadow != NULL) {
			dma_free(0, (ewokos_addr_t)shadow);
		}
		return 0;
	}

	if (shadow != NULL) {
		memcpy(buffer, shadow, size);
		dma_free(0, (ewokos_addr_t)shadow);
	}

	if (mailbox_call_with_alias(buffer, MAILBOX_VC_ALIAS_COHERENT, PROPERTY_CHANNEL) == 0) {
		if (alias_used != NULL) {
			*alias_used = MAILBOX_VC_ALIAS_COHERENT;
		}
		return 0;
	}
	return -1;
}

// 定义常用的 Mailbox 标签
#define TAG_SET_PHYS_SIZE   0x00048003
#define TAG_SET_VIRT_SIZE   0x00048004
#define TAG_SET_DEPTH       0x00048005
#define TAG_SET_PIXEL_ORDER 0x00048006
#define TAG_ALLOCATE_FB     0x00040001
#define TAG_GET_PITCH       0x00040008
#define TAG_ALLOCATE_MEM    0x0003000C
#define TAG_LOCK_MEM        0x0003000D
#define TAG_UNLOCK_MEM      0x0003000E
#define TAG_FREE_MEM        0x0003000F
#define TAG_EXECUTE_CODE    0x00030010
#define TAG_BLIT_IMAGE      0x0004000A

// 分配GPU内存
uint32_t allocate_gpu_memory(uint32_t size, uint32_t alignment, uint32_t flags) {
	uint32_t* buffer = (uint32_t*)(dma_alloc(0, 10*4));
    
    buffer[0] = 10 * 4;
    buffer[1] = 0;
    
    buffer[2] = TAG_ALLOCATE_MEM;
    buffer[3] = 12;
    buffer[4] = 12;
    buffer[5] = size;
    buffer[6] = alignment;
    buffer[7] = flags;
    
    buffer[8] = 0;
    
	mail_message_t msg;
	msg.data = (dma_phy_addr(0, (ewokos_addr_t)buffer) + MAILBOX_VC_ALIAS_NONCACHED) >> 4;
	msg.channel = 8;
	bcm283x_mailbox_call(&msg);
    
    uint32_t ret = buffer[5]; // 返回内存句柄
	dma_free(0, (ewokos_addr_t)buffer);
	return ret;
}

// 锁定GPU内存，获取物理地址
uint32_t lock_gpu_memory(uint32_t handle) {
	uint32_t* buffer = (uint32_t*)(dma_alloc(0, 8*4));
    
    buffer[0] = 8 * 4;
    buffer[1] = 0;
    
    buffer[2] = TAG_LOCK_MEM;
    buffer[3] = 4;
    buffer[4] = 4;
    buffer[5] = handle;
    
    buffer[6] = 0;

	mail_message_t msg;
	msg.data = (dma_phy_addr(0, (ewokos_addr_t)buffer) + MAILBOX_VC_ALIAS_NONCACHED) >> 4;
	msg.channel = 8;
	bcm283x_mailbox_call(&msg);
    
    uint32_t ret = buffer[5]; // 返回物理地址
	dma_free(0, (ewokos_addr_t)buffer);
	return ret;
}

// 解锁GPU内存
void unlock_gpu_memory(uint32_t handle) {
	uint32_t* buffer = (uint32_t*)(dma_alloc(0, 8*4));
    
    buffer[0] = 8 * 4;
    buffer[1] = 0;
    
    buffer[2] = TAG_UNLOCK_MEM;
    buffer[3] = 4;
    buffer[4] = 4;
    buffer[5] = handle;
    
    buffer[6] = 0;
    
	mail_message_t msg;
	msg.data = (dma_phy_addr(0, (ewokos_addr_t)buffer) + MAILBOX_VC_ALIAS_NONCACHED) >> 4;
	msg.channel = 8;
	bcm283x_mailbox_call(&msg);
	dma_free(0, (ewokos_addr_t)buffer);
}

// 释放GPU内存
void free_gpu_memory(uint32_t handle) {
	uint32_t* buffer = (uint32_t*)(dma_alloc(0, 7*4));
    
    buffer[0] = 7 * 4;
    buffer[1] = 0;
    
    buffer[2] = TAG_FREE_MEM;
    buffer[3] = 4;
    buffer[4] = 4;
    buffer[5] = handle;
    
    buffer[6] = 0;
    
	mail_message_t msg;
	msg.data = (dma_phy_addr(0, (ewokos_addr_t)buffer) + MAILBOX_VC_ALIAS_NONCACHED) >> 4;
	msg.channel = 8;
	bcm283x_mailbox_call(&msg);
	dma_free(0, (ewokos_addr_t)buffer);
}

// 执行带alpha通道的BLT操作
void gpu_blt_with_alpha(
    uint32_t src_addr, uint32_t src_width, uint32_t src_height,
    uint32_t dst_addr, uint32_t dst_width, uint32_t dst_height,
    int src_x, int src_y, int dst_x, int dst_y,
    int width, int height, uint32_t flags) {
    
	uint32_t* buffer = (uint32_t*)(dma_alloc(0, 24*4));
    
    buffer[0] = 24 * 4;
    buffer[1] = 0;
    
    buffer[2] = TAG_BLIT_IMAGE;
    buffer[3] = 64; // 值大小
    buffer[4] = 64; // 请求类型
    
    // 源矩形区域
    buffer[5] = src_x;
    buffer[6] = src_y;
    buffer[7] = src_x + width;
    buffer[8] = src_y + height;
    
    // 目标位置
    buffer[9] = dst_x;
    buffer[10] = dst_y;
    
    // 颜色空间和alpha选项
    buffer[11] = 0; // 源颜色空间 (0=RGB)
    buffer[12] = 0; // 目标颜色空间
    buffer[13] = flags; // 操作标志 (包含alpha选项)
    
    // 源缓冲区信息
    buffer[14] = src_addr;
    buffer[15] = src_width;
    buffer[16] = src_height;
    buffer[17] = src_width * 4; // 行字节数 (RGBA)
    
    // 目标缓冲区信息
    buffer[18] = dst_addr;
    buffer[19] = dst_width;
    buffer[20] = dst_height;
    buffer[21] = dst_width * 4; // 行字节数 (RGBA)
    
    buffer[22] = 0; // 结束标记
    
	mail_message_t msg;
	msg.data = (dma_phy_addr(0, (ewokos_addr_t)buffer) + MAILBOX_VC_ALIAS_NONCACHED) >> 4;
	msg.channel = 8;
	bcm283x_mailbox_call(&msg);
	dma_free(0, (ewokos_addr_t)buffer);
}

// 填充屏幕为单一颜色
void fill_screen(uint32_t color) {
	uint32_t* buffer = (uint32_t*)(dma_alloc(0, 24*4));
    
    buffer[0] = 23 * 4;
    buffer[1] = 0;
    
    buffer[2] = TAG_BLIT_IMAGE;
    buffer[3] = 36; // 值大小
    buffer[4] = 36; // 请求类型
    
    // 源矩形区域 (填充操作不需要实际源数据)
    buffer[5] = 0;
    buffer[6] = 0;
    buffer[7] = _fb_info.width;
    buffer[8] = _fb_info.height;
    
    // 目标位置
    buffer[9] = 0;
    buffer[10] = 0;
    
    // 颜色空间和操作选项 (1=填充颜色)
    buffer[11] = 0;
    buffer[12] = 0;
    buffer[13] = 1;
    
    // 源缓冲区信息 (填充颜色)
    buffer[14] = color;
    buffer[15] = 0;
    buffer[16] = 0;
    buffer[17] = 0;
    
    // 目标缓冲区信息
    buffer[18] = _fb_info.phy_base;
    buffer[19] = _fb_info.width;
    buffer[20] = _fb_info.height;
    buffer[21] = _fb_info.pitch;
    
    buffer[22] = 0; // 结束标记

	mail_message_t msg;
	msg.data = (dma_phy_addr(0, (ewokos_addr_t)buffer) + MAILBOX_VC_ALIAS_NONCACHED) >> 4;
	msg.channel = 8;
	bcm283x_mailbox_call(&msg);
	dma_free(0, (ewokos_addr_t)buffer);
}

// 主函数示例
void test(void) {
    // 填充屏幕为蓝色
    fill_screen(0xFF0000FF); // ARGB格式：不透明蓝色
    
    /*// 分配GPU内存用于源图像
    uint32_t src_handle = allocate_gpu_memory(200 * 200 * 4, 16, 0xC); // 可缓存内存
	klog("src handle: %x", src_handle);
    if (!src_handle) {
        while (1);
    }
    
    // 锁定内存获取物理地址
    uint32_t src_phys_addr = lock_gpu_memory(src_handle);
    if (!src_phys_addr) {
        free_gpu_memory(src_handle);
        while (1);
    }
	klog("src_phys_addr: %x", src_phys_addr);
    
    // 获取CPU可访问的地址
    uint32_t* src_cpu_addr = (uint32_t*)(src_phys_addr + 0xC0000000);
	klog("src_cpu_addr: %x", src_cpu_addr);
    
    // 创建一个简单的测试图案 (红色圆形带alpha渐变)
    for (int y = 0; y < 200; y++) {
        for (int x = 0; x < 200; x++) {
            int dx = x - 100;
            int dy = y - 100;
            int distance_squared = dx * dx + dy * dy;
            
            if (distance_squared <= 100 * 100) {
                // 计算距离边缘的距离，用于alpha渐变
                int distance_to_edge = 100 - (int)__builtin_sqrt(distance_squared);
                uint8_t alpha = (uint8_t)(distance_to_edge * 255 / 100);
                
                // ARGB格式：红色带alpha渐变
                src_cpu_addr[y * 200 + x] = (alpha << 24) | (0xFF << 16);
            } else {
                // 透明
                src_cpu_addr[y * 200 + x] = 0x00000000;
            }
        }
    }
    
    // 执行带alpha通道的BLT操作，将圆形绘制到屏幕中心
    gpu_blt_with_alpha(
        src_phys_addr, 200, 200,           // 源图像信息
        _fb_info.phy_base,           // 目标地址（帧缓冲）
        _fb_info.width, _fb_info.height, // 目标尺寸
        0, 0,                              // 源矩形起点
        300, 200,                          // 目标位置
        200, 200,                          // 宽度和高度
        0x01000000 | 0x00000001           // 标志：启用alpha，SRC_OVER混合模式
    );
    
    // 解锁并释放GPU内存
    unlock_gpu_memory(src_handle);
    free_gpu_memory(src_handle);
    
    // 进入无限循环，保持程序运行
	*/
}

typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t depth;
} fb_mode_t;

static int fb_mode_equal(const fb_mode_t* a, const fb_mode_t* b) {
	return a->width == b->width &&
			a->height == b->height &&
			a->depth == b->depth;
}

static void fb_build_request(uint32_t* req, uint32_t w, uint32_t h, uint32_t dep) {
	memset(req, 0, 30 * sizeof(uint32_t));
	req[0] = 30 * sizeof(uint32_t);
	req[1] = 0;
	req[2] = TAG_SET_PHYS_SIZE;
	req[3] = 8;
	req[4] = 8;
	req[5] = w;
	req[6] = h;
	req[7] = TAG_SET_VIRT_SIZE;
	req[8] = 8;
	req[9] = 8;
	req[10] = w;
	req[11] = h;
	req[12] = TAG_SET_DEPTH;
	req[13] = 4;
	req[14] = 4;
	req[15] = dep;
	req[16] = TAG_SET_PIXEL_ORDER;
	req[17] = 4;
	req[18] = 4;
	req[19] = PIXEL_ORDER_RGB;
	req[20] = TAG_ALLOCATE_FB;
	req[21] = 8;
	req[22] = 4;
	req[23] = 16;
	req[24] = 0;
	req[25] = TAG_GET_PITCH;
	req[26] = 4;
	req[27] = 0;
	req[28] = 0;
	req[29] = 0;
}

static int fb_try_mode(const sys_info_t* sysinfo, const fb_mode_t* mode, fbinfo_t* info) {
	uint32_t* req = (uint32_t*)dma_alloc(0, 30 * sizeof(uint32_t));
	uint32_t alias_used = 0;
	uint32_t resp_w;
	uint32_t resp_h;
	uint32_t resp_vw;
	uint32_t resp_vh;
	uint32_t resp_dep;
	uint32_t resp_phy;
	uint32_t resp_size;
	uint32_t resp_pitch;

	if (req == NULL) {
		return -1;
	}

	fb_build_request(req, mode->width, mode->height, mode->depth);
	if (mailbox_property_call_with_fallback(req, &alias_used) != 0) {
		klog("fb_init: mailbox timeout/failed %ux%ux%u\n",
				mode->width, mode->height, mode->depth);
		dma_free(0, (ewokos_addr_t)req);
		return -1;
	}

	resp_w = req[5];
	resp_h = req[6];
	resp_vw = req[10];
	resp_vh = req[11];
	resp_dep = req[15];
	resp_phy = req[23] & 0x3fffffff; // GPU addr to ARM addr
	resp_size = req[24];
	resp_pitch = req[28];
	dma_free(0, (ewokos_addr_t)req);

	if ((resp_w == 0) || (resp_h == 0) || (resp_phy == 0) || (resp_size == 0)) {
		klog("fb_init: bad reply %ux%ux%u alias=%x => w=%u h=%u phy=%x size=%u pitch=%u\n",
				mode->width, mode->height, mode->depth, alias_used,
				resp_w, resp_h, resp_phy, resp_size, resp_pitch);
		return -1;
	}

	if (resp_dep != 16 && resp_dep != 32) {
		klog("fb_init: unsupported depth %u for %ux%ux%u\n",
				resp_dep, mode->width, mode->height, mode->depth);
		return -1;
	}

	memset(info, 0, sizeof(fbinfo_t));
	info->width = resp_w;
	info->height = resp_h;
	info->vwidth = resp_vw != 0 ? resp_vw : resp_w;
	info->vheight = resp_vh != 0 ? resp_vh : resp_h;
	info->depth = resp_dep;
	info->pitch = resp_pitch != 0 ? resp_pitch : (info->vwidth * (info->depth / 8));
	info->phy_base = resp_phy;
	info->pointer = sysinfo->sys_dma.v_base + sysinfo->sys_dma.size;
	info->size = resp_size;
	info->xoffset = 0;
	info->yoffset = 0;
	info->size_max = align_up(resp_size, 4096);
	info->dma_id = -1;

	if (syscall3(SYS_MEM_MAP,
			(ewokos_addr_t)info->pointer,
			(ewokos_addr_t)info->phy_base,
			(ewokos_addr_t)info->size_max) == 0) {
		klog("fb_init: mem_map failed %ux%ux%u alias=%x v=%x phy=%x size=%u\n",
				mode->width, mode->height, mode->depth, alias_used,
				info->pointer, info->phy_base, info->size_max);
		memset(info, 0, sizeof(fbinfo_t));
		return -1;
	}
	return 0;
}

int32_t bcm283x_fb_init(uint32_t w, uint32_t h, uint32_t dep) {
	sys_info_t sysinfo;
	fb_mode_t requested;
	fb_mode_t fallbacks[] = {
		{1024, 768, 32},
		{800, 600, 32},
		{640, 480, 32},
		{640, 480, 16},
	};

	memset(&_fb_info, 0, sizeof(fbinfo_t));
	syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&sysinfo);

	bcm283x_mailbox_init();

	if (w == 0) {
		w = 1024;
	}
	if (h == 0) {
		h = 768;
	}
	if (dep != 16 && dep != 32) {
		dep = 32;
	}

	requested.width = w;
	requested.height = h;
	requested.depth = dep;

	if (fb_try_mode(&sysinfo, &requested, &_fb_info) == 0) {
		return 0;
	}

	for (uint32_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); ++i) {
		if (fb_mode_equal(&requested, &fallbacks[i])) {
			continue;
		}
		if (fb_try_mode(&sysinfo, &fallbacks[i], &_fb_info) == 0) {
			klog("fb_init: fallback %ux%ux%u -> %ux%ux%u\n",
					requested.width, requested.height, requested.depth,
					fallbacks[i].width, fallbacks[i].height, fallbacks[i].depth);
			return 0;
		}
	}
	klog("fb_init: all modes failed, last requested %ux%ux%u\n", w, h, dep);
	return -1;
}

fbinfo_t* bcm283x_get_fbinfo(void) {
	return &_fb_info;
}
