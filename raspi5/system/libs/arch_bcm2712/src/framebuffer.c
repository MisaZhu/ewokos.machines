#include <stdio.h>
#include <string.h>
#include <arch/bcm2712/mailbox.h>
#include <arch/bcm2712/framebuffer.h>
#include <ewoksys/syscall.h>
#include <ewoksys/klog.h>
#include <sysinfo.h>
#include <ewoksys/mmio.h>
#include <ewoksys/dma.h>

static fbinfo_t _fb_info;

#define PIXEL_ORDER_BGR 0u

#define TAG_SET_PHYS_SIZE   0x00048003
#define TAG_SET_VIRT_SIZE   0x00048004
#define TAG_SET_DEPTH       0x00048005
#define TAG_SET_PIXEL_ORDER 0x00048006
#define TAG_ALLOCATE_FB     0x00040001
#define TAG_GET_PITCH       0x00040008

/* GET variants: request_size=0, firmware returns the current value. */
#define TAG_GET_PHYS_SIZE   0x00040003
#define TAG_GET_VIRT_SIZE   0x00040004
#define TAG_GET_DEPTH       0x00040005
#define TAG_GET_PIXEL_ORDER 0x00040006

#define FB_REQ_WORDS 30

typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t depth;
} fb_mode_t;

static uint32_t align_up(uint32_t value, uint32_t align) {
	return (value + align - 1) & (~(align - 1));
}

static int mailbox_call_with_alias(uint32_t* buffer, uint32_t alias, uint8_t channel) {
	mail_message_t msg;
	memset(&msg, 0, sizeof(mail_message_t));
	/* Cast to uint32_t first: the legacy VPU mailbox only understands 32-bit
	 * bus addresses, so buffers must live in the lower-1GB aliasable range. */
	msg.data = ((uint32_t)dma_phy_addr(0, (ewokos_addr_t)buffer) | alias) >> 4;
	msg.channel = channel;
	if (bcm2712_mailbox_call_timeout(&msg, 0) != 0) {
		return -1;
	}
	return (buffer[1] & MAILBOX_RESPONSE_SUCCESS) != 0 ? 0 : -1;
}

static int mailbox_property_call_with_fallback(uint32_t* buffer) {
	uint32_t size = buffer[0];
	uint32_t* shadow = (uint32_t*)dma_alloc(0, size);

	if (shadow != NULL) {
		memcpy(shadow, buffer, size);
	}

	if (mailbox_call_with_alias(buffer, MAILBOX_VC_ALIAS_NONCACHED, PROPERTY_CHANNEL) == 0) {
		if (shadow != NULL) {
			dma_free(0, (ewokos_addr_t)shadow);
		}
		return 0;
	}

	if (shadow != NULL) {
		memcpy(buffer, shadow, size);
		dma_free(0, (ewokos_addr_t)shadow);
	}

	return mailbox_call_with_alias(buffer, MAILBOX_VC_ALIAS_COHERENT, PROPERTY_CHANNEL);
}

/*
 * Validate a firmware-reported mode and map the scanout buffer into this
 * process. Shared by all init strategies.
 */
static int fb_adopt(const sys_info_t* sysinfo, uint32_t w, uint32_t h,
		uint32_t vw, uint32_t vh, uint32_t dep,
		ewokos_addr_t phy, uint32_t size, uint32_t pitch, fbinfo_t* info) {
	if ((w == 0) || (h == 0) || (phy == 0) || (size == 0)) {
		return -1;
	}
	if (dep != 16 && dep != 32) {
		return -1;
	}

	memset(info, 0, sizeof(fbinfo_t));
	info->width = w;
	info->height = h;
	info->vwidth = vw != 0 ? vw : w;
	info->vheight = vh != 0 ? vh : h;
	info->depth = dep;
	info->pitch = pitch != 0 ? pitch : (info->vwidth * (info->depth / 8));
	info->phy_base = phy;
	info->pointer = sysinfo->sys_dma.v_base + sysinfo->sys_dma.size;
	info->size = size;
	info->xoffset = 0;
	info->yoffset = 0;
	info->size_max = align_up(size, 4096);
	info->dma_id = -1;

	if (syscall3(SYS_MEM_MAP,
			(ewokos_addr_t)info->pointer,
			(ewokos_addr_t)info->phy_base,
			(ewokos_addr_t)info->size_max) == 0) {
		klog("fb: mem_map fail v=%x phy=%x size=%u\n",
				info->pointer, info->phy_base, info->size_max);
		memset(info, 0, sizeof(fbinfo_t));
		return -1;
	}
	return 0;
}

static int fb_mode_equal(const fb_mode_t* a, const fb_mode_t* b) {
	return a->width == b->width &&
			a->height == b->height &&
			a->depth == b->depth;
}

static void fb_build_request(uint32_t* req, uint32_t w, uint32_t h, uint32_t dep) {
	memset(req, 0, FB_REQ_WORDS * sizeof(uint32_t));
	req[0] = FB_REQ_WORDS * sizeof(uint32_t);
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
	req[19] = PIXEL_ORDER_BGR;
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

/*
 * Strategy 1: allocate a new framebuffer via property tags (channel 8).
 * Works on firmware builds that still service the legacy FB allocator;
 * newer Pi5 firmware rejects it (response 0x80000001, zeroed tags).
 */
static int fb_try_mode(const sys_info_t* sysinfo, const fb_mode_t* mode, fbinfo_t* info) {
	uint32_t* req = (uint32_t*)dma_alloc(0, FB_REQ_WORDS * sizeof(uint32_t));
	uint32_t resp_w, resp_h, resp_vw, resp_vh;
	uint32_t resp_dep, resp_phy, resp_size, resp_pitch;
	int ret;

	if (req == NULL) {
		return -1;
	}

	fb_build_request(req, mode->width, mode->height, mode->depth);
	if (mailbox_property_call_with_fallback(req) != 0) {
		dma_free(0, (ewokos_addr_t)req);
		return -1;
	}

	resp_w = req[5];
	resp_h = req[6];
	resp_vw = req[10];
	resp_vh = req[11];
	resp_dep = req[15];
	/* Mask off VC bus alias bits to get the ARM physical address. */
	resp_phy = req[23] & 0x3fffffff;
	resp_size = req[24];
	resp_pitch = req[28];

	dma_free(0, (ewokos_addr_t)req);

	ret = fb_adopt(sysinfo, resp_w, resp_h, resp_vw, resp_vh, resp_dep,
			resp_phy, resp_size, resp_pitch, info);
	if (ret != 0) {
		klog("fb: alloc rejected %ux%ux%u\n",
				mode->width, mode->height, mode->depth);
	}
	return ret;
}

/*
 * Strategy 2: adopt the framebuffer the firmware already set up at boot.
 * GET tags plus ALLOCATE_FB with 0 request bytes query the existing
 * scanout buffer instead of allocating a new one.
 */
static int fb_query_existing(const sys_info_t* sysinfo, fbinfo_t* info) {
	uint32_t* req = (uint32_t*)dma_alloc(0, FB_REQ_WORDS * sizeof(uint32_t));
	uint32_t resp_w, resp_h, resp_vw, resp_vh;
	uint32_t resp_dep, resp_phy, resp_size, resp_pitch;
	int ret;

	if (req == NULL) {
		return -1;
	}

	memset(req, 0, FB_REQ_WORDS * sizeof(uint32_t));
	req[0] = FB_REQ_WORDS * sizeof(uint32_t);
	req[1] = 0;
	req[2] = TAG_GET_PHYS_SIZE;
	req[3] = 8;
	req[4] = 0;
	req[7] = TAG_GET_VIRT_SIZE;
	req[8] = 8;
	req[9] = 0;
	req[12] = TAG_GET_DEPTH;
	req[13] = 4;
	req[14] = 0;
	req[16] = TAG_GET_PIXEL_ORDER;
	req[17] = 4;
	req[18] = 0;
	req[20] = TAG_ALLOCATE_FB;
	req[21] = 8;
	req[22] = 0; /* 0 request bytes = query, don't allocate */
	req[25] = TAG_GET_PITCH;
	req[26] = 4;
	req[27] = 0;

	if (mailbox_property_call_with_fallback(req) != 0) {
		dma_free(0, (ewokos_addr_t)req);
		return -1;
	}

	resp_w = req[5];
	resp_h = req[6];
	resp_vw = req[10];
	resp_vh = req[11];
	resp_dep = req[15];
	resp_phy = req[23] & 0x3fffffff;
	resp_size = req[24];
	resp_pitch = req[28];

	dma_free(0, (ewokos_addr_t)req);

	ret = fb_adopt(sysinfo, resp_w, resp_h, resp_vw, resp_vh, resp_dep,
			resp_phy, resp_size, resp_pitch, info);
	if (ret != 0) {
		klog("fb: no boot fb (w=%u h=%u dep=%u phy=%x size=%u)\n",
				resp_w, resp_h, resp_dep, resp_phy, resp_size);
	}
	return ret;
}

/*
 * Strategy 3: legacy channel-1 simple framebuffer message. Predates the
 * property interface; the firmware fills in the response fields in place.
 */
typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t vwidth;
	uint32_t vheight;
	uint32_t pitch;
	uint32_t depth;
	uint32_t x;
	uint32_t y;
	uint32_t pointer;
	uint32_t size;
} __attribute__((aligned(16))) fb_ch1_msg_t;

static int fb_channel1_init(const sys_info_t* sysinfo,
		const fb_mode_t* mode, fbinfo_t* info) {
	fb_ch1_msg_t* msg = (fb_ch1_msg_t*)dma_alloc(0, sizeof(fb_ch1_msg_t));
	mail_message_t mailbox_msg;
	uint32_t w, h, vw, vh, dep, phy, size, pitch;
	int ret;

	if (msg == NULL) {
		return -1;
	}

	memset(msg, 0, sizeof(fb_ch1_msg_t));
	msg->width = mode->width;
	msg->height = mode->height;
	msg->vwidth = mode->width;
	msg->vheight = mode->height;
	msg->depth = mode->depth;

	memset(&mailbox_msg, 0, sizeof(mailbox_msg));
	mailbox_msg.data = ((uint32_t)dma_phy_addr(0, (ewokos_addr_t)msg)
			| MAILBOX_VC_ALIAS_NONCACHED) >> 4;
	mailbox_msg.channel = FRAMEBUFFER_CHANNEL;

	if (bcm2712_mailbox_call_timeout(&mailbox_msg, 0) != 0) {
		dma_free(0, (ewokos_addr_t)msg);
		return -1;
	}

	w = msg->width;
	h = msg->height;
	vw = msg->vwidth;
	vh = msg->vheight;
	dep = msg->depth;
	phy = msg->pointer & 0x3fffffff;
	size = msg->size;
	pitch = msg->pitch;

	dma_free(0, (ewokos_addr_t)msg);

	ret = fb_adopt(sysinfo, w, h, vw, vh, dep, phy, size, pitch, info);
	if (ret != 0) {
		klog("fb: ch1 rejected %ux%ux%u\n",
				mode->width, mode->height, mode->depth);
	}
	return ret;
}

int32_t bcm2712_fb_init(uint32_t w, uint32_t h, uint32_t dep) {
	sys_info_t sysinfo;
	fb_mode_t requested;
	fb_mode_t fallbacks[] = {
		{1024, 768, 32},
		{800, 600, 32},
		{640, 480, 32},
		{640, 480, 16},
	};
	const uint32_t n_fallbacks = sizeof(fallbacks) / sizeof(fallbacks[0]);

	memset(&_fb_info, 0, sizeof(fbinfo_t));
	syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&sysinfo);

	if (bcm2712_mailbox_init() == 0) {
		klog("fb_init: mmio map failed\n");
		return -1;
	}

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
		goto done;
	}

	/* Firmware may clamp the requested mode (config.txt hdmi_group/hdmi_mode
	 * wins on Pi5), so walk a table of safe modes. */
	for (uint32_t i = 0; i < n_fallbacks; ++i) {
		if (fb_mode_equal(&requested, &fallbacks[i])) {
			continue;
		}
		if (fb_try_mode(&sysinfo, &fallbacks[i], &_fb_info) == 0) {
			goto done;
		}
	}

	if (fb_query_existing(&sysinfo, &_fb_info) == 0) {
		goto done;
	}

	if (fb_channel1_init(&sysinfo, &requested, &_fb_info) == 0) {
		goto done;
	}

	for (uint32_t i = 0; i < n_fallbacks; ++i) {
		if (fb_mode_equal(&requested, &fallbacks[i])) {
			continue;
		}
		if (fb_channel1_init(&sysinfo, &fallbacks[i], &_fb_info) == 0) {
			goto done;
		}
	}

	klog("fb_init: all modes failed\n");
	return -1;

done:
	klog("fb_init: %ux%u@%u pitch=%u phy=%x size=%u\n",
			_fb_info.width, _fb_info.height, _fb_info.depth,
			_fb_info.pitch, _fb_info.phy_base, _fb_info.size);
	return 0;
}

fbinfo_t* bcm2712_get_fbinfo(void) {
	return &_fb_info;
}
