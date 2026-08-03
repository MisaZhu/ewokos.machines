#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/shm.h>
#include <ewoksys/dma.h>
#include <ewoksys/fbinfo.h>
#include <ewoksys/mmio.h>
#include <ewoksys/proto.h>
#include <ewoksys/vdevice.h>
#include <graph/graph.h>
#include <graph/uv12.h>
#include <bsp/bsp_fb.h>
#include "g2d.h"
#ifdef G2DD_ENABLE_MI_GFX
#include "mi_gfx.h"
#include "mi_sys.h"
#endif

typedef struct g2d_state g2d_state_t;

typedef struct {
	const char* name;
	uint32_t backend_id;
	int32_t (*setup)(g2d_state_t* state);
	void (*teardown)(g2d_state_t* state);
	int32_t (*clear)(g2d_state_t* state, uint32_t color);
	int32_t (*fill_rect)(g2d_state_t* state, const g2d_fill_req_t* req);
	int32_t (*blit)(g2d_state_t* state, const g2d_blit_req_t* req, const uint32_t* data, uint8_t use_alpha);
} g2d_backend_t;

typedef struct {
	ewokos_addr_t canvas_vaddr;
	uint32_t canvas_phy;
	uint32_t canvas_size;
	ewokos_addr_t src_vaddr;
	uint32_t src_phy;
	uint32_t src_size;
} g2d_ge_ctx_t;

#ifdef G2DD_ENABLE_MI_GFX
typedef struct {
	MI_PHY phy_addr;
	void* virt_addr;
	MI_U32 size;
	MI_BOOL sys_ready;
	MI_BOOL gfx_ready;
} g2d_mi_ctx_t;
#endif

struct g2d_state {
	fbinfo_t* fbinfo;
	graph_t* canvas;
	uint32_t clear_color;
	const g2d_backend_t* backend;
	g2d_ge_ctx_t ge;
#ifdef G2DD_ENABLE_MI_GFX
	g2d_mi_ctx_t mi;
#endif
};

static int32_t g2d_setup(g2d_state_t* state);
static void g2d_teardown(g2d_state_t* state);
static int32_t g2d_present(g2d_state_t* state);

static int32_t g2d_soft_setup(g2d_state_t* state);
static void g2d_soft_teardown(g2d_state_t* state);
static int32_t g2d_soft_clear(g2d_state_t* state, uint32_t color);
static int32_t g2d_soft_fill_rect(g2d_state_t* state, const g2d_fill_req_t* req);
static int32_t g2d_soft_blit(g2d_state_t* state, const g2d_blit_req_t* req, const uint32_t* data, uint8_t use_alpha);

static int32_t g2d_ge_setup(g2d_state_t* state);
static void g2d_ge_teardown(g2d_state_t* state);
static int32_t g2d_ge_clear(g2d_state_t* state, uint32_t color);
static int32_t g2d_ge_fill_rect(g2d_state_t* state, const g2d_fill_req_t* req);
static int32_t g2d_ge_blit(g2d_state_t* state, const g2d_blit_req_t* req, const uint32_t* data, uint8_t use_alpha);

static int32_t g2d_blit_attach(const g2d_blit_req_t* req, graph_t* src, graph_t** owned_graph, void** shm_ptr);
static void g2d_blit_release(graph_t* owned_graph, void* shm_ptr);

static const g2d_backend_t g_g2d_ge_backend = {
	"ssd20xd-ge",
	G2D_BACKEND_SSD20XD_GE,
	g2d_ge_setup,
	g2d_ge_teardown,
	g2d_ge_clear,
	g2d_ge_fill_rect,
	g2d_ge_blit
};

static const g2d_backend_t g_g2d_soft_backend = {
	"soft-nv12",
	G2D_BACKEND_SOFT_NV12,
	g2d_soft_setup,
	g2d_soft_teardown,
	g2d_soft_clear,
	g2d_soft_fill_rect,
	g2d_soft_blit
};

#ifdef G2DD_ENABLE_MI_GFX
static int32_t g2d_mi_setup(g2d_state_t* state);
static void g2d_mi_teardown(g2d_state_t* state);
static int32_t g2d_mi_clear(g2d_state_t* state, uint32_t color);
static int32_t g2d_mi_fill_rect(g2d_state_t* state, const g2d_fill_req_t* req);
static int32_t g2d_mi_blit(g2d_state_t* state, const g2d_blit_req_t* req, const uint32_t* data, uint8_t use_alpha);

static const g2d_backend_t g_g2d_mi_backend = {
	"mi_gfx",
	G2D_BACKEND_MI_GFX,
	g2d_mi_setup,
	g2d_mi_teardown,
	g2d_mi_clear,
	g2d_mi_fill_rect,
	g2d_mi_blit
};
#endif

static int32_t g2d_setup_framebuffer(g2d_state_t* state) {
	if(state == NULL)
		return -1;

	if(bsp_fb_init(640, 480, 16) != 0)
		return -1;

	state->fbinfo = bsp_get_fbinfo();
	if(state->fbinfo == NULL ||
			state->fbinfo->width == 0 ||
			state->fbinfo->height == 0 ||
			state->fbinfo->pointer == 0) {
		return -1;
	}
	return 0;
}

static int32_t g2d_setup(g2d_state_t* state) {
	if(g2d_setup_framebuffer(state) != 0)
		return -1;

	state->clear_color = 0xff000000;

	if(g_g2d_ge_backend.setup(state) == 0) {
		state->backend = &g_g2d_ge_backend;
		return 0;
	}

#ifdef G2DD_ENABLE_MI_GFX
	if(g_g2d_mi_backend.setup(state) == 0) {
		state->backend = &g_g2d_mi_backend;
		return 0;
	}
#endif

	if(g_g2d_soft_backend.setup(state) != 0)
		return -1;
	state->backend = &g_g2d_soft_backend;
	return 0;
}

static void g2d_teardown(g2d_state_t* state) {
	if(state == NULL)
		return;
	if(state->backend != NULL && state->backend->teardown != NULL)
		state->backend->teardown(state);
	state->backend = NULL;
}

/*
 * SSD20XD GE lives on the RIU bus and must be programmed through 16-bit word
 * accesses. Buffer addresses use the MIU-visible bus view, not the ARM's
 * physical address window.
 */
#define MIYOO_MMIO_PHY_BASE      0x1f000000U
#define MIYOO_MIU_PHY_BASE       0x20000000U
#define SSD20XD_GE_PHY_BASE      0x1f281200U

#define GE_REG_CTRL              0x000U
#define GE_REG_CTRL1             0x004U
#define GE_REG_STATUS            0x01cU
#define GE_REG_IRQ               0x078U
#define GE_REG_ABL_COEF          0x044U
#define GE_REG_ABL_CONST         0x04cU
#define GE_REG_SRC_BASE          0x080U
#define GE_REG_DST_BASE          0x098U
#define GE_REG_SRC_PITCH         0x0c0U
#define GE_REG_DST_PITCH         0x0ccU
#define GE_REG_COLORFMT          0x0d0U
#define GE_REG_CLIP_LEFT         0x154U
#define GE_REG_CLIP_RIGHT        0x158U
#define GE_REG_CLIP_TOP          0x15cU
#define GE_REG_CLIP_BOTTOM       0x160U
#define GE_REG_ROT               0x164U
#define GE_REG_CMD               0x180U
#define GE_REG_X0                0x1a0U
#define GE_REG_Y0                0x1a4U
#define GE_REG_X1                0x1a8U
#define GE_REG_Y1                0x1acU
#define GE_REG_X2                0x1b0U
#define GE_REG_Y2                0x1b4U
#define GE_REG_SRC_WIDTH         0x1b8U
#define GE_REG_SRC_HEIGHT        0x1bcU
#define GE_REG_COLOR             0x1c0U

#define GE_CTRL_ENABLE           (1U << 0)
#define GE_CTRL_ABL              (1U << 2)
#define GE_CTRL_DFB              (1U << 10)
#define GE_CTRL1_CLK_EN          (1U << 15)

#define GE_STATUS_BUSY           (1U << 0)
#define GE_IRQ_CLR_MASK          (3U << 10)

#define GE_FMT_ARGB8888          0x000fU

#define GE_CMD_RECTFILL          (3U << 4)
#define GE_CMD_BITBLT            (4U << 4)

static inline uint32_t g2d_ge_miu_addr(uint32_t phy_addr) {
	if(phy_addr >= MIYOO_MIU_PHY_BASE)
		return phy_addr - MIYOO_MIU_PHY_BASE;
	return phy_addr;
}

static inline uint32_t g2d_ge_addr(uint32_t reg) {
	return _mmio_base + (SSD20XD_GE_PHY_BASE - MIYOO_MMIO_PHY_BASE) + reg;
}

static inline uint16_t g2d_ge_read16(uint32_t reg) {
	return *((volatile uint16_t*)g2d_ge_addr(reg));
}

static inline void g2d_ge_write16(uint32_t reg, uint16_t value) {
	*((volatile uint16_t*)g2d_ge_addr(reg)) = value;
}

static inline void g2d_ge_write32(uint32_t reg, uint32_t value) {
	g2d_ge_write16(reg, (uint16_t)(value & 0xffffU));
	g2d_ge_write16(reg + 2U, (uint16_t)((value >> 16) & 0xffffU));
}

static int32_t g2d_ge_wait_idle(uint32_t timeout_us) {
	while(timeout_us > 0) {
		if((g2d_ge_read16(GE_REG_STATUS) & GE_STATUS_BUSY) == 0)
			return 0;
		usleep(1);
		timeout_us--;
	}
	return -1;
}

static void g2d_ge_irq_clear(void) {
	g2d_ge_write16(GE_REG_IRQ, GE_IRQ_CLR_MASK);
	g2d_ge_write16(GE_REG_IRQ, 0);
}

static int32_t g2d_ge_rect_valid(g2d_state_t* state, int32_t x, int32_t y, int32_t w, int32_t h) {
	if(state == NULL || state->canvas == NULL)
		return 0;
	if(x < 0 || y < 0 || w <= 0 || h <= 0)
		return 0;
	if((x + w) > state->canvas->w || (y + h) > state->canvas->h)
		return 0;
	return 1;
}

static void g2d_ge_set_clip(g2d_state_t* state, int32_t x, int32_t y, int32_t w, int32_t h) {
	if(state == NULL || state->canvas == NULL)
		return;

	if(x < 0)
		x = 0;
	if(y < 0)
		y = 0;
	if(w <= 0 || h <= 0) {
		x = 0;
		y = 0;
		w = state->canvas->w;
		h = state->canvas->h;
	}
	if((x + w) > state->canvas->w)
		w = state->canvas->w - x;
	if((y + h) > state->canvas->h)
		h = state->canvas->h - y;

	g2d_ge_write16(GE_REG_CLIP_LEFT, (uint16_t)x);
	g2d_ge_write16(GE_REG_CLIP_RIGHT, (uint16_t)(x + w - 1));
	g2d_ge_write16(GE_REG_CLIP_TOP, (uint16_t)y);
	g2d_ge_write16(GE_REG_CLIP_BOTTOM, (uint16_t)(y + h - 1));
}

static void g2d_ge_reset_state(g2d_state_t* state, uint8_t enable_alpha, uint8_t alpha) {
	uint16_t ctrl = GE_CTRL_ENABLE;
	uint16_t ctrl1 = g2d_ge_read16(GE_REG_CTRL1);

	if(enable_alpha != 0) {
		ctrl |= GE_CTRL_ABL | GE_CTRL_DFB;
		g2d_ge_write16(GE_REG_ABL_COEF, 0x0001U);
		g2d_ge_write16(GE_REG_ABL_CONST, (uint16_t)alpha | ((uint16_t)alpha << 8));
	}
	else {
		g2d_ge_write16(GE_REG_ABL_COEF, 0);
		g2d_ge_write16(GE_REG_ABL_CONST, 0);
	}

	ctrl1 |= GE_CTRL1_CLK_EN;
	g2d_ge_write16(GE_REG_CTRL1, ctrl1);
	g2d_ge_write16(GE_REG_CTRL, ctrl);
	g2d_ge_write16(GE_REG_CTRL + 2U, 0);
	g2d_ge_write16(GE_REG_ROT, 0);
	g2d_ge_write16(GE_REG_COLORFMT, GE_FMT_ARGB8888 | (GE_FMT_ARGB8888 << 8));
	g2d_ge_set_clip(state, 0, 0, state->canvas->w, state->canvas->h);
	g2d_ge_irq_clear();
}

static int32_t g2d_ge_ensure_src(g2d_state_t* state, uint32_t size) {
	uint32_t aligned_size;
	ewokos_addr_t vaddr;

	if(state == NULL || size == 0)
		return -1;
	aligned_size = (size + 31U) & ~31U;
	if(state->ge.src_vaddr != 0 && state->ge.src_size >= aligned_size)
		return 0;

	if(state->ge.src_vaddr != 0) {
		dma_free(0, state->ge.src_vaddr);
		state->ge.src_vaddr = 0;
		state->ge.src_phy = 0;
		state->ge.src_size = 0;
	}

	vaddr = dma_alloc(0, aligned_size);
	if(vaddr == 0)
		return -1;
	state->ge.src_vaddr = vaddr;
	state->ge.src_phy = dma_phy_addr(0, vaddr);
	state->ge.src_size = aligned_size;
	if(state->ge.src_phy == 0) {
		dma_free(0, state->ge.src_vaddr);
		state->ge.src_vaddr = 0;
		state->ge.src_size = 0;
		return -1;
	}
	return 0;
}

static int32_t g2d_ge_setup(g2d_state_t* state) {
	ewokos_addr_t vaddr;

	if(state == NULL || state->fbinfo == NULL)
		return -1;

	if(_mmio_base == 0 && mmio_map() == 0)
		return -1;

	memset(&state->ge, 0, sizeof(state->ge));
	state->ge.canvas_size = state->fbinfo->width * state->fbinfo->height * 4;
	vaddr = dma_alloc(0, state->ge.canvas_size);
	if(vaddr == 0)
		return -1;

	state->ge.canvas_vaddr = vaddr;
	state->ge.canvas_phy = dma_phy_addr(0, vaddr);
	if(state->ge.canvas_phy == 0) {
		dma_free(0, state->ge.canvas_vaddr);
		memset(&state->ge, 0, sizeof(state->ge));
		return -1;
	}

	state->canvas = graph_new((uint32_t*)state->ge.canvas_vaddr, state->fbinfo->width, state->fbinfo->height);
	if(state->canvas == NULL) {
		dma_free(0, state->ge.canvas_vaddr);
		memset(&state->ge, 0, sizeof(state->ge));
		return -1;
	}

	if(g2d_ge_wait_idle(2000) != 0) {
		g2d_ge_teardown(state);
		return -1;
	}

	g2d_ge_reset_state(state, 0, 0xff);
	if(g2d_ge_clear(state, state->clear_color) != 0)
		graph_clear(state->canvas, state->clear_color);
	return 0;
}

static void g2d_ge_teardown(g2d_state_t* state) {
	if(state == NULL)
		return;
	if(state->canvas != NULL) {
		graph_free(state->canvas);
		state->canvas = NULL;
	}
	if(state->ge.src_vaddr != 0)
		dma_free(0, state->ge.src_vaddr);
	if(state->ge.canvas_vaddr != 0)
		dma_free(0, state->ge.canvas_vaddr);
	memset(&state->ge, 0, sizeof(state->ge));
}

static int32_t g2d_ge_clear(g2d_state_t* state, uint32_t color) {
	g2d_fill_req_t req;

	if(state == NULL || state->canvas == NULL)
		return -1;

	req.rect.x = 0;
	req.rect.y = 0;
	req.rect.w = state->canvas->w;
	req.rect.h = state->canvas->h;
	req.color = color;
	if(g2d_ge_fill_rect(state, &req) != 0)
		graph_clear(state->canvas, color);
	state->clear_color = color;
	return 0;
}

static int32_t g2d_ge_fill_rect(g2d_state_t* state, const g2d_fill_req_t* req) {
	uint32_t dst_miu;

	if(state == NULL || state->canvas == NULL || req == NULL)
		return -1;
	if(g2d_ge_rect_valid(state, req->rect.x, req->rect.y, req->rect.w, req->rect.h) == 0)
		return g2d_soft_fill_rect(state, req);
	if(g2d_ge_wait_idle(2000) != 0)
		return g2d_soft_fill_rect(state, req);

	dst_miu = g2d_ge_miu_addr(state->ge.canvas_phy);
	g2d_ge_reset_state(state, 0, 0xff);
	g2d_ge_write32(GE_REG_DST_BASE, dst_miu);
	g2d_ge_write16(GE_REG_DST_PITCH, (uint16_t)(state->canvas->w * 4));
	g2d_ge_write32(GE_REG_COLOR, req->color);
	g2d_ge_write16(GE_REG_X0, (uint16_t)req->rect.x);
	g2d_ge_write16(GE_REG_Y0, (uint16_t)req->rect.y);
	g2d_ge_write16(GE_REG_X1, (uint16_t)(req->rect.x + req->rect.w - 1));
	g2d_ge_write16(GE_REG_Y1, (uint16_t)(req->rect.y + req->rect.h - 1));
	g2d_ge_write16(GE_REG_CMD, GE_CMD_RECTFILL);
	if(g2d_ge_wait_idle(20000) != 0)
		return g2d_soft_fill_rect(state, req);
	return 0;
}

static int32_t g2d_ge_blit(g2d_state_t* state, const g2d_blit_req_t* req, const uint32_t* data, uint8_t use_alpha) {
	uint32_t src_size;
	uint32_t src_miu;
	uint32_t dst_miu;

	if(state == NULL || state->canvas == NULL || req == NULL || data == NULL)
		return -1;
	if(use_alpha != 0)
		return g2d_soft_blit(state, req, data, use_alpha);
	if(req->sw != req->dw || req->sh != req->dh)
		return g2d_soft_blit(state, req, data, use_alpha);
	if(g2d_ge_rect_valid(state, req->dx, req->dy, req->dw, req->dh) == 0)
		return g2d_soft_blit(state, req, data, use_alpha);
	if(req->sx < 0 || req->sy < 0 || req->sw <= 0 || req->sh <= 0)
		return g2d_soft_blit(state, req, data, use_alpha);
	if((req->sx + req->sw) > (int32_t)req->src_w || (req->sy + req->sh) > (int32_t)req->src_h)
		return g2d_soft_blit(state, req, data, use_alpha);

	src_size = req->src_w * req->src_h * 4;
	if(g2d_ge_ensure_src(state, src_size) != 0)
		return g2d_soft_blit(state, req, data, use_alpha);

	memcpy((void*)state->ge.src_vaddr, data, src_size);
	src_miu = g2d_ge_miu_addr(state->ge.src_phy);
	dst_miu = g2d_ge_miu_addr(state->ge.canvas_phy);
	if(g2d_ge_wait_idle(2000) != 0)
		return g2d_soft_blit(state, req, data, use_alpha);

	g2d_ge_reset_state(state, 0, 0xff);
	g2d_ge_write32(GE_REG_SRC_BASE, src_miu);
	g2d_ge_write16(GE_REG_SRC_PITCH, (uint16_t)(req->src_w * 4));
	g2d_ge_write32(GE_REG_DST_BASE, dst_miu);
	g2d_ge_write16(GE_REG_DST_PITCH, (uint16_t)(state->canvas->w * 4));
	g2d_ge_write16(GE_REG_SRC_WIDTH, (uint16_t)req->src_w);
	g2d_ge_write16(GE_REG_SRC_HEIGHT, (uint16_t)req->src_h);
	g2d_ge_write16(GE_REG_X0, (uint16_t)req->dx);
	g2d_ge_write16(GE_REG_Y0, (uint16_t)req->dy);
	g2d_ge_write16(GE_REG_X1, (uint16_t)(req->dx + req->dw - 1));
	g2d_ge_write16(GE_REG_Y1, (uint16_t)(req->dy + req->dh - 1));
	g2d_ge_write16(GE_REG_X2, (uint16_t)req->sx);
	g2d_ge_write16(GE_REG_Y2, (uint16_t)req->sy);
	g2d_ge_write16(GE_REG_CMD, GE_CMD_BITBLT);
	if(g2d_ge_wait_idle(20000) != 0)
		return g2d_soft_blit(state, req, data, use_alpha);
	return 0;
}

static int32_t g2d_soft_setup(g2d_state_t* state) {
	state->canvas = graph_new(NULL, state->fbinfo->width, state->fbinfo->height);
	if(state->canvas == NULL || state->canvas->buffer == NULL) {
		if(state->canvas != NULL) {
			graph_free(state->canvas);
			state->canvas = NULL;
		}
		return -1;
	}
	graph_clear(state->canvas, state->clear_color);
	return 0;
}

static void g2d_soft_teardown(g2d_state_t* state) {
	if(state->canvas != NULL) {
		graph_free(state->canvas);
		state->canvas = NULL;
	}
}

static int32_t g2d_soft_clear(g2d_state_t* state, uint32_t color) {
	if(state == NULL || state->canvas == NULL)
		return -1;
	state->clear_color = color;
	graph_clear(state->canvas, color);
	return 0;
}

static int32_t g2d_soft_fill_rect(g2d_state_t* state, const g2d_fill_req_t* req) {
	if(state == NULL || state->canvas == NULL || req == NULL)
		return -1;
	graph_fill_rect(state->canvas,
			req->rect.x, req->rect.y,
			req->rect.w, req->rect.h,
			req->color);
	return 0;
}

static int32_t g2d_soft_blit(g2d_state_t* state, const g2d_blit_req_t* req, const uint32_t* data, uint8_t use_alpha) {
	graph_t src;

	if(state == NULL || state->canvas == NULL || req == NULL || data == NULL)
		return -1;

	memset(&src, 0, sizeof(src));
	graph_init(&src, data, req->src_w, req->src_h);
	if(use_alpha != 0) {
		graph_blt_alpha(&src,
				req->sx, req->sy, req->sw, req->sh,
				state->canvas,
				req->dx, req->dy, req->dw, req->dh,
				req->alpha);
	}
	else {
		graph_blt(&src,
				req->sx, req->sy, req->sw, req->sh,
				state->canvas,
				req->dx, req->dy, req->dw, req->dh);
	}
	return 0;
}

static int32_t g2d_blit_attach(const g2d_blit_req_t* req, graph_t* src, graph_t** owned_graph, void** shm_ptr) {
	uint8_t* shm;
	uint32_t stride;
	uint32_t expected;
	uint32_t y;
	graph_t* packed;

	if(req == NULL || src == NULL || owned_graph == NULL || shm_ptr == NULL)
		return -1;
	if(req->src_shm_id < 0 || req->src_w == 0 || req->src_h == 0 || req->src_format != G2D_FMT_ARGB8888)
		return -1;

	expected = req->src_w * req->src_h * 4;
	stride = req->src_stride;
	if(stride == 0)
		stride = req->src_w * 4;
	if(stride < req->src_w * 4)
		return -1;
	if(req->src_size < expected || req->src_size < stride * req->src_h)
		return -1;

	shm = shmat(req->src_shm_id, 0, 0);
	if(shm == (void*)-1)
		return -1;

	*owned_graph = NULL;
	*shm_ptr = shm;
	if(stride == req->src_w * 4) {
		memset(src, 0, sizeof(*src));
		graph_init(src, (const uint32_t*)shm, req->src_w, req->src_h);
		return 0;
	}

	packed = graph_new(NULL, req->src_w, req->src_h);
	if(packed == NULL || packed->buffer == NULL) {
		if(packed != NULL)
			graph_free(packed);
		shmdt(shm);
		*shm_ptr = NULL;
		return -1;
	}

	for(y = 0; y < req->src_h; y++) {
		memcpy(((uint8_t*)packed->buffer) + y * req->src_w * 4,
				shm + y * stride,
				req->src_w * 4);
	}
	*owned_graph = packed;
	*src = *packed;
	return 0;
}

static void g2d_blit_release(graph_t* owned_graph, void* shm_ptr) {
	if(owned_graph != NULL)
		graph_free(owned_graph);
	if(shm_ptr != NULL)
		shmdt(shm_ptr);
}

static int32_t g2d_present(g2d_state_t* state) {
	if(state == NULL || state->fbinfo == NULL || state->canvas == NULL)
		return -1;
	rgb2nv12((uint8_t*)state->fbinfo->pointer,
			state->canvas->buffer,
			state->canvas->w,
			state->canvas->h);
	return 0;
}

#ifdef G2DD_ENABLE_MI_GFX
static MI_U8 g_g2d_heap_name[] = "mma_heap_name0";

typedef struct {
	MI_PHY phy_addr;
	void* virt_addr;
	MI_U32 size;
} g2d_mi_buffer_t;

static void g2d_mi_rect(MI_GFX_Rect_t* rect, int32_t x, int32_t y, int32_t w, int32_t h) {
	if(rect == NULL)
		return;
	rect->s32Xpos = x;
	rect->s32Ypos = y;
	rect->u32Width = w > 0 ? (MI_U32)w : 0;
	rect->u32Height = h > 0 ? (MI_U32)h : 0;
}

static void g2d_mi_surface(g2d_state_t* state, MI_GFX_Surface_t* surface) {
	if(state == NULL || surface == NULL)
		return;
	memset(surface, 0, sizeof(*surface));
	surface->phyAddr = state->mi.phy_addr;
	surface->eColorFmt = E_MI_GFX_FMT_ARGB8888;
	surface->u32Width = state->canvas->w;
	surface->u32Height = state->canvas->h;
	surface->u32Stride = state->canvas->w * 4;
}

static int32_t g2d_mi_wait(MI_U16 fence) {
	if(MI_GFX_WaitAllDone(MI_TRUE, fence) != MI_SUCCESS)
		return -1;
	return 0;
}

static int32_t g2d_mi_alloc_buffer(g2d_mi_buffer_t* buffer, MI_U32 size) {
	if(buffer == NULL || size == 0)
		return -1;

	memset(buffer, 0, sizeof(*buffer));
	if(MI_SYS_MMA_Alloc(g_g2d_heap_name, size, &buffer->phy_addr) != MI_SUCCESS)
		return -1;
	if(MI_SYS_Mmap(buffer->phy_addr, size, &buffer->virt_addr, MI_FALSE) != MI_SUCCESS) {
		MI_SYS_MMA_Free(buffer->phy_addr);
		memset(buffer, 0, sizeof(*buffer));
		return -1;
	}
	buffer->size = size;
	return 0;
}

static void g2d_mi_free_buffer(g2d_mi_buffer_t* buffer) {
	if(buffer == NULL)
		return;
	if(buffer->virt_addr != NULL)
		MI_SYS_Munmap(buffer->virt_addr, buffer->size);
	if(buffer->phy_addr != 0)
		MI_SYS_MMA_Free(buffer->phy_addr);
	memset(buffer, 0, sizeof(*buffer));
}

static int32_t g2d_mi_is_simple_rect(const g2d_blit_req_t* req) {
	if(req == NULL)
		return 0;
	if(req->sx < 0 || req->sy < 0 || req->dx < 0 || req->dy < 0)
		return 0;
	if(req->sw <= 0 || req->sh <= 0 || req->dw <= 0 || req->dh <= 0)
		return 0;
	if(req->sw != req->dw || req->sh != req->dh)
		return 0;
	return 1;
}

static int32_t g2d_mi_setup(g2d_state_t* state) {
	if(state == NULL)
		return -1;

	memset(&state->mi, 0, sizeof(state->mi));
	if(MI_SYS_Init() != MI_SUCCESS)
		return -1;
	state->mi.sys_ready = MI_TRUE;

	if(MI_GFX_Open() != MI_SUCCESS) {
		MI_SYS_Exit();
		memset(&state->mi, 0, sizeof(state->mi));
		return -1;
	}
	state->mi.gfx_ready = MI_TRUE;

	state->mi.size = state->fbinfo->width * state->fbinfo->height * 4;
	if(MI_SYS_MMA_Alloc(g_g2d_heap_name, state->mi.size, &state->mi.phy_addr) != MI_SUCCESS) {
		g2d_mi_teardown(state);
		return -1;
	}

	if(MI_SYS_Mmap(state->mi.phy_addr, state->mi.size, &state->mi.virt_addr, MI_FALSE) != MI_SUCCESS) {
		g2d_mi_teardown(state);
		return -1;
	}

	state->canvas = graph_new((uint32_t*)state->mi.virt_addr, state->fbinfo->width, state->fbinfo->height);
	if(state->canvas == NULL) {
		g2d_mi_teardown(state);
		return -1;
	}

	if(g2d_mi_clear(state, state->clear_color) != 0)
		graph_clear(state->canvas, state->clear_color);
	return 0;
}

static void g2d_mi_teardown(g2d_state_t* state) {
	if(state == NULL)
		return;
	if(state->canvas != NULL) {
		graph_free(state->canvas);
		state->canvas = NULL;
	}
	if(state->mi.virt_addr != NULL) {
		MI_SYS_Munmap(state->mi.virt_addr, state->mi.size);
		state->mi.virt_addr = NULL;
	}
	if(state->mi.phy_addr != 0) {
		MI_SYS_MMA_Free(state->mi.phy_addr);
		state->mi.phy_addr = 0;
	}
	if(state->mi.gfx_ready == MI_TRUE)
		MI_GFX_Close();
	if(state->mi.sys_ready == MI_TRUE)
		MI_SYS_Exit();
	memset(&state->mi, 0, sizeof(state->mi));
}

static int32_t g2d_mi_clear(g2d_state_t* state, uint32_t color) {
	MI_GFX_Surface_t surface;
	MI_GFX_Rect_t rect;
	MI_U16 fence = 0;

	if(state == NULL || state->canvas == NULL)
		return -1;

	g2d_mi_surface(state, &surface);
	g2d_mi_rect(&rect, 0, 0, state->canvas->w, state->canvas->h);
	if(MI_GFX_QuickFill(&surface, &rect, color, &fence) != MI_SUCCESS || g2d_mi_wait(fence) != 0) {
		graph_clear(state->canvas, color);
	}
	state->clear_color = color;
	return 0;
}

static int32_t g2d_mi_fill_rect(g2d_state_t* state, const g2d_fill_req_t* req) {
	MI_GFX_Surface_t surface;
	MI_GFX_Rect_t rect;
	MI_U16 fence = 0;

	if(state == NULL || state->canvas == NULL || req == NULL)
		return -1;
	if(req->rect.w <= 0 || req->rect.h <= 0 || req->rect.x < 0 || req->rect.y < 0)
		return g2d_soft_fill_rect(state, req);

	g2d_mi_surface(state, &surface);
	g2d_mi_rect(&rect, req->rect.x, req->rect.y, req->rect.w, req->rect.h);
	if(MI_GFX_QuickFill(&surface, &rect, req->color, &fence) != MI_SUCCESS || g2d_mi_wait(fence) != 0)
		return g2d_soft_fill_rect(state, req);
	return 0;
}

static int32_t g2d_mi_blit(g2d_state_t* state, const g2d_blit_req_t* req, const uint32_t* data, uint8_t use_alpha) {
	MI_GFX_Surface_t src_surface;
	MI_GFX_Surface_t dst_surface;
	MI_GFX_Rect_t src_rect;
	MI_GFX_Rect_t dst_rect;
	MI_GFX_Opt_t opt;
	MI_U16 fence = 0;
	g2d_mi_buffer_t src_buffer;
	MI_U32 expected;

	if(state == NULL || state->canvas == NULL || req == NULL || data == NULL)
		return -1;
	if(g2d_mi_is_simple_rect(req) == 0)
		return g2d_soft_blit(state, req, data, use_alpha);

	expected = req->src_w * req->src_h * 4;
	if(g2d_mi_alloc_buffer(&src_buffer, expected) != 0)
		return g2d_soft_blit(state, req, data, use_alpha);

	memcpy(src_buffer.virt_addr, data, expected);
	MI_SYS_FlushInvCache(src_buffer.virt_addr, expected);

	memset(&src_surface, 0, sizeof(src_surface));
	src_surface.phyAddr = src_buffer.phy_addr;
	src_surface.eColorFmt = E_MI_GFX_FMT_ARGB8888;
	src_surface.u32Width = req->src_w;
	src_surface.u32Height = req->src_h;
	src_surface.u32Stride = req->src_w * 4;

	g2d_mi_surface(state, &dst_surface);
	g2d_mi_rect(&src_rect, req->sx, req->sy, req->sw, req->sh);
	g2d_mi_rect(&dst_rect, req->dx, req->dy, req->dw, req->dh);

	memset(&opt, 0, sizeof(opt));
	g2d_mi_rect(&opt.stClipRect, 0, 0, state->canvas->w, state->canvas->h);
	opt.eMirror = E_MI_GFX_MIRROR_NONE;
	opt.eRotate = E_MI_GFX_ROTATE_0;
	if(use_alpha != 0) {
		opt.eSrcDfbBldOp = E_MI_GFX_DFB_BLD_SRCALPHA;
		opt.eDstDfbBldOp = E_MI_GFX_DFB_BLD_INVSRCALPHA;
		opt.eDFBBlendFlag = E_MI_GFX_DFB_BLEND_COLORALPHA | E_MI_GFX_DFB_BLEND_ALPHACHANNEL;
		opt.u32GlobalSrcConstColor = ((MI_U32)req->alpha) << 24;
	}
	else {
		opt.eSrcDfbBldOp = E_MI_GFX_DFB_BLD_ONE;
		opt.eDstDfbBldOp = E_MI_GFX_DFB_BLD_ZERO;
		opt.eDFBBlendFlag = E_MI_GFX_DFB_BLEND_NOFX;
	}

	if(MI_GFX_BitBlit(&src_surface, &src_rect, &dst_surface, &dst_rect, &opt, &fence) != MI_SUCCESS ||
			g2d_mi_wait(fence) != 0) {
		g2d_mi_free_buffer(&src_buffer);
		return g2d_soft_blit(state, req, data, use_alpha);
	}

	g2d_mi_free_buffer(&src_buffer);
	return 0;
}
#endif

static int32_t g2d_get_info(proto_t* ret, g2d_state_t* state) {
	g2d_info_t info;

	if(ret == NULL || state == NULL || state->canvas == NULL)
		return -1;

	memset(&info, 0, sizeof(info));
	info.width = state->canvas->w;
	info.height = state->canvas->h;
	info.depth = 32;
	info.format = G2D_FMT_ARGB8888;
	info.backend = state->backend != NULL ? state->backend->backend_id : G2D_BACKEND_SOFT_NV12;
	PF->init(ret)->add(ret, &info, sizeof(info));
	return 0;
}

static int32_t g2d_clear(proto_t* in, g2d_state_t* state) {
	uint32_t color;

	if(in == NULL || state == NULL || state->canvas == NULL)
		return -1;
	if(proto_read_to(in, &color, sizeof(color)) != sizeof(color))
		return -1;
	if(state->backend == NULL || state->backend->clear == NULL)
		return -1;
	return state->backend->clear(state, color);
}

static int32_t g2d_fill_rect(proto_t* in, g2d_state_t* state) {
	g2d_fill_req_t req;

	if(in == NULL || state == NULL || state->canvas == NULL)
		return -1;
	if(proto_read_to(in, &req, sizeof(req)) != sizeof(req))
		return -1;
	if(state->backend == NULL || state->backend->fill_rect == NULL)
		return -1;
	return state->backend->fill_rect(state, &req);
}

static int32_t g2d_do_blit(proto_t* in, g2d_state_t* state, uint8_t use_alpha) {
	g2d_blit_req_t req;
	graph_t src;
	graph_t* owned_graph;
	void* shm_ptr;
	int32_t ret;

	if(in == NULL || state == NULL || state->canvas == NULL)
		return -1;
	if(proto_read_to(in, &req, sizeof(req)) != sizeof(req))
		return -1;
	if(req.src_format != G2D_FMT_ARGB8888 || req.src_w == 0 || req.src_h == 0)
		return -1;
	if(state->backend == NULL || state->backend->blit == NULL)
		return -1;

	memset(&src, 0, sizeof(src));
	owned_graph = NULL;
	shm_ptr = NULL;
	if(g2d_blit_attach(&req, &src, &owned_graph, &shm_ptr) != 0)
		return -1;

	ret = state->backend->blit(state, &req, src.buffer, use_alpha);
	g2d_blit_release(owned_graph, shm_ptr);
	return ret;
}

static char* g2d_strdup(const char* s) {
	size_t len;
	char* ret;

	if(s == NULL)
		return NULL;
	len = strlen(s);
	ret = (char*)malloc(len + 1);
	if(ret == NULL)
		return NULL;
	memcpy(ret, s, len + 1);
	return ret;
}

static char* g2d_cmd(vdevice_t* dev, int from_pid, int argc, char** argv, void* p) {
	(void)dev;
	(void)from_pid;
	g2d_state_t* state = (g2d_state_t*)p;

	if(argc <= 0 || argv == NULL || argv[0] == NULL || state == NULL)
		return NULL;

	if(strcmp(argv[0], "info") == 0) {
		static char info[96];
		snprintf(info, sizeof(info), "%dx%d argb8888 via %s",
				state->canvas->w, state->canvas->h,
				state->backend != NULL ? state->backend->name : "unknown");
		return g2d_strdup(info);
	}
	if(strcmp(argv[0], "present") == 0) {
		g2d_present(state);
		return g2d_strdup("ok");
	}
	if(strcmp(argv[0], "clear") == 0 && argc > 1) {
		uint32_t color = (uint32_t)strtoul(argv[1], NULL, 0);
		if(state->backend != NULL && state->backend->clear != NULL)
			state->backend->clear(state, color);
		return g2d_strdup("ok");
	}
	return NULL;
}

static int g2d_dev_cntl(vdevice_t* dev, int from_pid, int cmd, proto_t* in, proto_t* ret, void* p) {
	(void)dev;
	(void)from_pid;
	g2d_state_t* state = (g2d_state_t*)p;

	if(state == NULL)
		return -1;

	switch(cmd) {
	case G2D_DEV_CNTL_GET_INFO:
		return g2d_get_info(ret, state);
	case G2D_DEV_CNTL_CLEAR:
		return g2d_clear(in, state);
	case G2D_DEV_CNTL_FILL_RECT:
		return g2d_fill_rect(in, state);
	case G2D_DEV_CNTL_BLIT:
		return g2d_do_blit(in, state, 0);
	case G2D_DEV_CNTL_BLIT_ALPHA:
		return g2d_do_blit(in, state, 1);
	case G2D_DEV_CNTL_PRESENT:
		return g2d_present(state);
	default:
		return -1;
	}
}

int main(int argc, char** argv) {
	const char* mnt_point = argc > 1 ? argv[1] : "/dev/g2d";
	g2d_state_t state;
	vdevice_t dev;

	memset(&state, 0, sizeof(state));
	if(g2d_setup(&state) != 0)
		return -1;

	memset(&dev, 0, sizeof(dev));
	strcpy(dev.name, "g2d");
	dev.dev_cntl = g2d_dev_cntl;
	dev.cmd = g2d_cmd;
	dev.extra_data = &state;

	if(device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666) != 0) {
		g2d_teardown(&state);
		return -1;
	}

	g2d_teardown(&state);
	return 0;
}
