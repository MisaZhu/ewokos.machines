#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/shm.h>
#include <ewoksys/fbinfo.h>
#include <ewoksys/proto.h>
#include <ewoksys/vdevice.h>
#include <graph/graph.h>
#include <bsp/bsp_fb.h>
#include <tinyjson/tinyjson.h>
#include "g2d.h"
#include "vc_g2d.h"

/*
 * raspix g2dd - 2D acceleration daemon for Raspberry Pi 3/4.
 *
 * External interface is identical to the miyoo g2dd driver and matches
 * system/gui/libs/g2d: clients issue dev_cntl commands on /dev/g2d and,
 * like with the framebuffer driver, pass image contents through shared
 * memory (g2d_blit_req_t.src_shm_id).
 *
 * Rendering happens on an ARGB8888 canvas held in a DMA-coherent buffer so
 * the VideoCore can read/write it directly; G2D_DEV_CNTL_PRESENT flushes the
 * canvas to the framebuffer (hardware blit when the fb is 32bpp).
 */

typedef struct g2d_state g2d_state_t;

typedef struct {
	const char* name;
	uint32_t backend_id;
	int32_t (*clear)(g2d_state_t* state, uint32_t color);
	int32_t (*fill_rect)(g2d_state_t* state, const g2d_fill_req_t* req);
	int32_t (*blit)(g2d_state_t* state, const g2d_blit_req_t* req, const uint32_t* data, uint8_t use_alpha);
} g2d_backend_t;

struct g2d_state {
	fbinfo_t* fbinfo;
	graph_t* canvas;
	vc_g2d_buf_t canvas_buf; /* DMA buffer backing the canvas, VC-accessible */
	vc_g2d_buf_t fb_buf;     /* framebuffer wrapped as VC destination */
	uint32_t clear_color;
	int32_t vc_ready;
	const g2d_backend_t* backend;
};

static int32_t g2d_present(g2d_state_t* state);

static int32_t g2d_soft_clear(g2d_state_t* state, uint32_t color);
static int32_t g2d_soft_fill_rect(g2d_state_t* state, const g2d_fill_req_t* req);
static int32_t g2d_soft_blit(g2d_state_t* state, const g2d_blit_req_t* req, const uint32_t* data, uint8_t use_alpha);

static int32_t g2d_vc_clear(g2d_state_t* state, uint32_t color);
static int32_t g2d_vc_fill_rect(g2d_state_t* state, const g2d_fill_req_t* req);
static int32_t g2d_vc_blit(g2d_state_t* state, const g2d_blit_req_t* req, const uint32_t* data, uint8_t use_alpha);

static int32_t g2d_blit_attach(const g2d_blit_req_t* req, graph_t* src, graph_t** owned_graph, void** shm_ptr);
static void g2d_blit_release(graph_t* owned_graph, void* shm_ptr);

static const g2d_backend_t g_g2d_soft_backend = {
	"soft",
	G2D_BACKEND_SOFT_NV12,
	g2d_soft_clear,
	g2d_soft_fill_rect,
	g2d_soft_blit
};

static const g2d_backend_t g_g2d_vc_backend = {
	"vc_mailbox",
	G2D_BACKEND_VC_MAILBOX,
	g2d_vc_clear,
	g2d_vc_fill_rect,
	g2d_vc_blit
};

static int32_t g2d_setup_framebuffer(g2d_state_t* state, uint32_t w, uint32_t h, uint32_t dep) {
	if (state == NULL)
		return -1;

	if (bsp_fb_init(w, h, dep) != 0)
		return -1;

	state->fbinfo = bsp_get_fbinfo();
	if (state->fbinfo == NULL ||
			state->fbinfo->width == 0 ||
			state->fbinfo->height == 0 ||
			state->fbinfo->pointer == 0) {
		return -1;
	}
	return 0;
}

static int32_t g2d_setup(g2d_state_t* state, uint32_t w, uint32_t h, uint32_t dep) {
	uint32_t size;

	if (g2d_setup_framebuffer(state, w, h, dep) != 0)
		return -1;

	state->clear_color = 0xff000000;

	/* canvas: non-cached DMA buffer, coherent between CPU and VideoCore */
	size = state->fbinfo->width * state->fbinfo->height * 4;
	if (vc_g2d_buf_alloc(&state->canvas_buf, size) != 0)
		return -1;

	state->canvas = graph_new((uint32_t*)(uintptr_t)state->canvas_buf.vaddr,
			state->fbinfo->width, state->fbinfo->height);
	if (state->canvas == NULL) {
		vc_g2d_buf_free(&state->canvas_buf);
		return -1;
	}

	vc_g2d_buf_wrap(&state->fb_buf, state->fbinfo->pointer, state->fbinfo->phy_base);

	/* probe the VC blitter with a full-canvas fill (also the initial clear) */
	state->vc_ready = 0;
	if (vc_g2d_init() == 0 &&
			vc_g2d_fill(&state->canvas_buf,
				state->fbinfo->width, state->fbinfo->height,
				state->fbinfo->width * 4,
				0, 0, state->fbinfo->width, state->fbinfo->height,
				state->clear_color) == 0) {
		state->vc_ready = 1;
	}

	if (state->vc_ready != 0)
		state->backend = &g_g2d_vc_backend;
	else {
		graph_clear(state->canvas, state->clear_color);
		state->backend = &g_g2d_soft_backend;
	}
	return 0;
}

static void g2d_teardown(g2d_state_t* state) {
	if (state == NULL)
		return;
	if (state->canvas != NULL) {
		graph_free(state->canvas);
		state->canvas = NULL;
	}
	vc_g2d_buf_free(&state->canvas_buf);
	state->backend = NULL;
	state->vc_ready = 0;
}

static int32_t g2d_soft_clear(g2d_state_t* state, uint32_t color) {
	if (state == NULL || state->canvas == NULL)
		return -1;
	state->clear_color = color;
	graph_clear(state->canvas, color);
	return 0;
}

static int32_t g2d_soft_fill_rect(g2d_state_t* state, const g2d_fill_req_t* req) {
	if (state == NULL || state->canvas == NULL || req == NULL)
		return -1;
	graph_fill_rect(state->canvas,
			req->rect.x, req->rect.y,
			req->rect.w, req->rect.h,
			req->color);
	return 0;
}

static int32_t g2d_soft_blit(g2d_state_t* state, const g2d_blit_req_t* req, const uint32_t* data, uint8_t use_alpha) {
	graph_t src;

	if (state == NULL || state->canvas == NULL || req == NULL || data == NULL)
		return -1;

	memset(&src, 0, sizeof(src));
	graph_init(&src, data, req->src_w, req->src_h);
	if (use_alpha != 0) {
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

static int32_t g2d_vc_clear(g2d_state_t* state, uint32_t color) {
	if (state == NULL || state->canvas == NULL)
		return -1;
	if (vc_g2d_fill(&state->canvas_buf,
			(uint32_t)state->canvas->w, (uint32_t)state->canvas->h,
			(uint32_t)state->canvas->w * 4,
			0, 0, state->canvas->w, state->canvas->h,
			color) != 0) {
		graph_clear(state->canvas, color);
	}
	state->clear_color = color;
	return 0;
}

static int32_t g2d_vc_fill_rect(g2d_state_t* state, const g2d_fill_req_t* req) {
	int32_t w;
	int32_t h;

	if (state == NULL || state->canvas == NULL || req == NULL)
		return -1;
	if (req->rect.w <= 0 || req->rect.h <= 0 || req->rect.x < 0 || req->rect.y < 0)
		return g2d_soft_fill_rect(state, req);

	/* the VC blitter can't clip against the canvas; clamp here */
	w = req->rect.w;
	h = req->rect.h;
	if (req->rect.x + w > state->canvas->w)
		w = state->canvas->w - req->rect.x;
	if (req->rect.y + h > state->canvas->h)
		h = state->canvas->h - req->rect.y;
	if (w <= 0 || h <= 0)
		return 0;

	if (vc_g2d_fill(&state->canvas_buf,
			(uint32_t)state->canvas->w, (uint32_t)state->canvas->h,
			(uint32_t)state->canvas->w * 4,
			req->rect.x, req->rect.y, w, h,
			req->color) != 0)
		return g2d_soft_fill_rect(state, req);
	return 0;
}

static int32_t g2d_vc_blit(g2d_state_t* state, const g2d_blit_req_t* req, const uint32_t* data, uint8_t use_alpha) {
	vc_g2d_buf_t src;
	uint32_t size;
	uint32_t flags;
	int32_t ret;

	if (state == NULL || state->canvas == NULL || req == NULL || data == NULL)
		return -1;

	/* cases the VC blitter can't express go through the CPU path */
	if (req->sx < 0 || req->sy < 0 || req->dx < 0 || req->dy < 0 ||
			req->sw <= 0 || req->sh <= 0 || req->dw <= 0 || req->dh <= 0)
		return g2d_soft_blit(state, req, data, use_alpha);
	if (req->sx + req->sw > (int32_t)req->src_w ||
			req->sy + req->sh > (int32_t)req->src_h ||
			req->dx + req->dw > state->canvas->w ||
			req->dy + req->dh > state->canvas->h)
		return g2d_soft_blit(state, req, data, use_alpha);
	/* global-alpha modulation isn't expressible; per-pixel alpha only */
	if (use_alpha != 0 && req->alpha != 0xff)
		return g2d_soft_blit(state, req, data, use_alpha);

	size = req->src_w * req->src_h * 4;
	if (vc_g2d_buf_alloc(&src, size) != 0)
		return g2d_soft_blit(state, req, data, use_alpha);

	/* non-cached staging: CPU copy lands in DRAM, visible to the VC as-is */
	memcpy((void*)(uintptr_t)src.vaddr, data, size);

	flags = use_alpha != 0 ? VC_G2D_BLIT_ALPHA : VC_G2D_BLIT_OPAQUE;
	ret = vc_g2d_blit(&src,
			req->src_w, req->src_h, req->src_w * 4,
			req->sx, req->sy, req->sw, req->sh,
			&state->canvas_buf,
			(uint32_t)state->canvas->w, (uint32_t)state->canvas->h,
			(uint32_t)state->canvas->w * 4,
			req->dx, req->dy, req->dw, req->dh,
			flags);
	vc_g2d_buf_free(&src);

	if (ret != 0)
		return g2d_soft_blit(state, req, data, use_alpha);
	return 0;
}

static int32_t g2d_blit_attach(const g2d_blit_req_t* req, graph_t* src, graph_t** owned_graph, void** shm_ptr) {
	uint8_t* shm;
	uint32_t stride;
	uint32_t expected;
	uint32_t y;
	graph_t* packed;

	if (req == NULL || src == NULL || owned_graph == NULL || shm_ptr == NULL)
		return -1;
	if (req->src_shm_id < 0 || req->src_w == 0 || req->src_h == 0 || req->src_format != G2D_FMT_ARGB8888)
		return -1;

	expected = req->src_w * req->src_h * 4;
	stride = req->src_stride;
	if (stride == 0)
		stride = req->src_w * 4;
	if (stride < req->src_w * 4)
		return -1;
	if (req->src_size < expected || req->src_size < stride * req->src_h)
		return -1;

	shm = shmat(req->src_shm_id, 0, 0);
	if (shm == NULL)
		return -1;

	*owned_graph = NULL;
	*shm_ptr = shm;
	if (stride == req->src_w * 4) {
		memset(src, 0, sizeof(*src));
		graph_init(src, (const uint32_t*)shm, req->src_w, req->src_h);
		return 0;
	}

	packed = graph_new(NULL, req->src_w, req->src_h);
	if (packed == NULL || packed->buffer == NULL) {
		if (packed != NULL)
			graph_free(packed);
		shmdt(shm);
		*shm_ptr = NULL;
		return -1;
	}

	for (y = 0; y < req->src_h; y++) {
		memcpy(((uint8_t*)packed->buffer) + y * req->src_w * 4,
				shm + y * stride,
				req->src_w * 4);
	}
	*owned_graph = packed;
	*src = *packed;
	return 0;
}

static void g2d_blit_release(graph_t* owned_graph, void* shm_ptr) {
	if (owned_graph != NULL)
		graph_free(owned_graph);
	if (shm_ptr != NULL)
		shmdt(shm_ptr);
}

static uint32_t g2d_present_soft32(g2d_state_t* state) {
	fbinfo_t* fbinfo = state->fbinfo;
	uint8_t* dst = (uint8_t*)(uintptr_t)(fbinfo->pointer +
			fbinfo->yoffset * fbinfo->pitch +
			fbinfo->xoffset * 4);
	const uint32_t* src = state->canvas->buffer;
	uint32_t row_bytes = (uint32_t)state->canvas->w * 4;
	uint32_t total_bytes = (uint32_t)state->canvas->h * row_bytes;
	int32_t y;

	if (fbinfo->pitch == row_bytes) {
		memcpy(dst, src, total_bytes);
		return total_bytes;
	}

	for (y = 0; y < state->canvas->h; ++y) {
		memcpy(dst + y * fbinfo->pitch,
				(const uint8_t*)(src + y * state->canvas->w),
				row_bytes);
	}
	return total_bytes;
}

static uint32_t g2d_present_soft16(g2d_state_t* state) {
	fbinfo_t* fbinfo = state->fbinfo;
	uint8_t* dst_base = (uint8_t*)(uintptr_t)fbinfo->pointer +
			fbinfo->yoffset * fbinfo->pitch +
			fbinfo->xoffset * 2;
	const uint32_t* src = state->canvas->buffer;
	int32_t x;
	int32_t y;

	for (y = 0; y < state->canvas->h; ++y) {
		const uint32_t* s = src + y * state->canvas->w;
		uint16_t* d = (uint16_t*)(dst_base + y * fbinfo->pitch);
		for (x = 0; x < state->canvas->w; ++x) {
			uint32_t c = s[x];
			uint8_t r = (c >> 16) & 0xff;
			uint8_t g = (c >> 8) & 0xff;
			uint8_t b = c & 0xff;
			d[x] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
		}
	}
	return (uint32_t)state->canvas->w * (uint32_t)state->canvas->h * 2;
}

static int32_t g2d_present(g2d_state_t* state) {
	fbinfo_t* fbinfo;

	if (state == NULL || state->fbinfo == NULL || state->canvas == NULL)
		return -1;
	fbinfo = state->fbinfo;

	if (fbinfo->depth == 32) {
		if (state->vc_ready != 0 &&
				vc_g2d_blit(&state->canvas_buf,
					(uint32_t)state->canvas->w, (uint32_t)state->canvas->h,
					(uint32_t)state->canvas->w * 4,
					0, 0, state->canvas->w, state->canvas->h,
					&state->fb_buf,
					fbinfo->width, fbinfo->height, fbinfo->pitch,
					(int32_t)fbinfo->xoffset, (int32_t)fbinfo->yoffset,
					state->canvas->w, state->canvas->h,
					VC_G2D_BLIT_OPAQUE) == 0)
			return 0;
		g2d_present_soft32(state);
		return 0;
	}

	if (fbinfo->depth == 16) {
		g2d_present_soft16(state);
		return 0;
	}
	return -1;
}

static int32_t g2d_get_info(proto_t* ret, g2d_state_t* state) {
	g2d_info_t info;

	if (ret == NULL || state == NULL || state->canvas == NULL)
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

	if (in == NULL || state == NULL || state->canvas == NULL)
		return -1;
	if (proto_read_to(in, &color, sizeof(color)) != sizeof(color))
		return -1;
	if (state->backend == NULL || state->backend->clear == NULL)
		return -1;
	return state->backend->clear(state, color);
}

static int32_t g2d_fill_rect(proto_t* in, g2d_state_t* state) {
	g2d_fill_req_t req;

	if (in == NULL || state == NULL || state->canvas == NULL)
		return -1;
	if (proto_read_to(in, &req, sizeof(req)) != sizeof(req))
		return -1;
	if (state->backend == NULL || state->backend->fill_rect == NULL)
		return -1;
	return state->backend->fill_rect(state, &req);
}

static int32_t g2d_do_blit(proto_t* in, g2d_state_t* state, uint8_t use_alpha) {
	g2d_blit_req_t req;
	graph_t src;
	graph_t* owned_graph;
	void* shm_ptr;
	int32_t ret;

	if (in == NULL || state == NULL || state->canvas == NULL)
		return -1;
	if (proto_read_to(in, &req, sizeof(req)) != sizeof(req))
		return -1;
	if (req.src_format != G2D_FMT_ARGB8888 || req.src_w == 0 || req.src_h == 0)
		return -1;
	if (state->backend == NULL || state->backend->blit == NULL)
		return -1;

	memset(&src, 0, sizeof(src));
	owned_graph = NULL;
	shm_ptr = NULL;
	if (g2d_blit_attach(&req, &src, &owned_graph, &shm_ptr) != 0)
		return -1;

	ret = state->backend->blit(state, &req, src.buffer, use_alpha);
	g2d_blit_release(owned_graph, shm_ptr);
	return ret;
}

static char* g2d_strdup(const char* s) {
	size_t len;
	char* ret;

	if (s == NULL)
		return NULL;
	len = strlen(s);
	ret = (char*)malloc(len + 1);
	if (ret == NULL)
		return NULL;
	memcpy(ret, s, len + 1);
	return ret;
}

static char* g2d_cmd(vdevice_t* dev, int from_pid, int argc, char** argv, void* p) {
	(void)dev;
	(void)from_pid;
	g2d_state_t* state = (g2d_state_t*)p;

	if (argc <= 0 || argv == NULL || argv[0] == NULL || state == NULL)
		return NULL;

	if (strcmp(argv[0], "info") == 0) {
		static char info[96];
		snprintf(info, sizeof(info), "%dx%d argb8888 via %s",
				state->canvas->w, state->canvas->h,
				state->backend != NULL ? state->backend->name : "unknown");
		return g2d_strdup(info);
	}
	if (strcmp(argv[0], "present") == 0) {
		g2d_present(state);
		return g2d_strdup("ok");
	}
	if (strcmp(argv[0], "clear") == 0 && argc > 1) {
		uint32_t color = (uint32_t)strtoul(argv[1], NULL, 0);
		if (state->backend != NULL && state->backend->clear != NULL)
			state->backend->clear(state, color);
		return g2d_strdup("ok");
	}
	return NULL;
}

static int g2d_dev_cntl(vdevice_t* dev, int from_pid, int cmd, proto_t* in, proto_t* ret, void* p) {
	(void)dev;
	(void)from_pid;
	g2d_state_t* state = (g2d_state_t*)p;

	if (state == NULL)
		return -1;

	switch (cmd) {
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

static void read_config(const char* conf_file, uint32_t* w, uint32_t* h, uint32_t* dep) {
	if (conf_file == NULL || conf_file[0] == 0)
		conf_file = "/etc/framebuffer.json";
	json_var_t* conf_var = json_parse_file(conf_file);

	/* request the same mode fbd uses, so the VC framebuffer stays put */
	*w = (uint32_t)json_get_int_def(conf_var, "width", 0);
	*h = (uint32_t)json_get_int_def(conf_var, "height", 0);
	*dep = (uint32_t)json_get_int_def(conf_var, "depth", 32);

	if (conf_var != NULL)
		json_var_unref(conf_var);
}

static int doargs(int argc, char* argv[], const char** conf_file) {
	int c = 0;
	while (c != -1) {
		c = getopt(argc, argv, "c:");
		if (c == -1)
			break;

		switch (c) {
		case 'c':
			*conf_file = optarg;
			break;
		default:
			c = -1;
			break;
		}
	}
	return optind;
}

int main(int argc, char** argv) {
	const char* conf_file = "/etc/framebuffer.json";
	g2d_state_t state;
	vdevice_t dev;
	uint32_t w = 0;
	uint32_t h = 0;
	uint32_t dep = 32;

	int opti = doargs(argc, argv, &conf_file);
	const char* mnt_point = (opti < argc && opti >= 0) ? argv[opti] : "/dev/g2d";

	read_config(conf_file, &w, &h, &dep);

	memset(&state, 0, sizeof(state));
	if (g2d_setup(&state, w, h, dep) != 0)
		return -1;

	memset(&dev, 0, sizeof(dev));
	strcpy(dev.name, "g2d");
	dev.dev_cntl = g2d_dev_cntl;
	dev.cmd = g2d_cmd;
	dev.extra_data = &state;

	if (device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666) != 0) {
		g2d_teardown(&state);
		return -1;
	}

	g2d_teardown(&state);
	return 0;
}
