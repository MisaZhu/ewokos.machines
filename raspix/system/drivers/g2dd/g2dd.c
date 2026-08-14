#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/shm.h>
#include <ewoksys/klog.h>
#include <ewoksys/proto.h>
#include <ewoksys/vdevice.h>
#include <tinyjson/tinyjson.h>
#include "g2d.h"
#include "vc4_kms_v3d.h"

/*
 * raspix g2dd - strict offscreen 2D accelerator for Raspberry Pi 3/4.
 *
 * The daemon owns one offscreen ARGB8888 render surface and exposes 2D ops on
 * it through /dev/g2d. Display presentation is intentionally out of scope:
 * callers can validate results through GET_PIXEL or hand the rendered surface
 * to a separate display path.
 */

typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t depth;
	uint32_t format;
} g2d_surface_t;

typedef struct {
	const uint32_t* pixels;
	void* shm_ptr;
	uint32_t* packed_pixels;
} g2d_blit_source_t;

typedef struct {
	g2d_surface_t surface;
	vc4_kms_v3d_t vc4;
	g2d_stats_t stats;
} g2d_state_t;

static int32_t g2d_surface_init(g2d_surface_t* surface, uint32_t width, uint32_t height, uint32_t depth) {
	if (surface == NULL || width == 0 || height == 0 || depth != 32)
		return -1;

	memset(surface, 0, sizeof(*surface));
	surface->width = width;
	surface->height = height;
	surface->depth = depth;
	surface->format = G2D_FMT_ARGB8888;
	return 0;
}

static void g2d_surface_teardown(g2d_surface_t* surface) {
	if (surface == NULL)
		return;
	memset(surface, 0, sizeof(*surface));
}

static int32_t g2d_surface_read_pixel(const g2d_state_t* state, int32_t x, int32_t y, uint32_t* pixel) {
	if (state == NULL || pixel == NULL)
		return -1;
	if (x < 0 || y < 0 ||
			x >= (int32_t)state->surface.width ||
			y >= (int32_t)state->surface.height) {
		return -1;
	}
	return vc4_kms_v3d_read_pixel((vc4_kms_v3d_t*)&state->vc4, x, y, pixel);
}

static int32_t g2d_state_setup(g2d_state_t* state, uint32_t width, uint32_t height, uint32_t depth) {
	if (state == NULL)
		return -1;
	if (g2d_surface_init(&state->surface, width, height, depth) != 0) {
		klog("g2dd: surface_init failed w=%u h=%u depth=%u\n", width, height, depth);
		return -1;
	}
	if (vc4_kms_v3d_init(&state->vc4, width, height, depth) != 0) {
		klog("g2dd: vc4 init failed last_error=%d, strict VC4-only mode\n",
				state->vc4.last_error);
		g2d_surface_teardown(&state->surface);
		return -1;
	}
	return 0;
}

static void g2d_state_teardown(g2d_state_t* state) {
	if (state == NULL)
		return;
	vc4_kms_v3d_teardown(&state->vc4);
	g2d_surface_teardown(&state->surface);
	memset(&state->stats, 0, sizeof(state->stats));
}

static int32_t g2d_blit_source_attach(const g2d_blit_req_t* req, g2d_blit_source_t* src) {
	uint8_t* shm;
	uint32_t stride;
	uint32_t expected;
	uint32_t y;

	if (req == NULL || src == NULL)
		return -1;
	if (req->src_shm_id < 0 || req->src_w == 0 || req->src_h == 0 || req->src_format != G2D_FMT_ARGB8888)
		return -1;

	memset(src, 0, sizeof(*src));
	expected = req->src_w * req->src_h * 4;
	stride = req->src_stride != 0 ? req->src_stride : req->src_w * 4;
	if (stride < req->src_w * 4)
		return -1;
	if (req->src_size < expected || req->src_size < stride * req->src_h)
		return -1;

	shm = shmat(req->src_shm_id, 0, 0);
	if (shm == (void*)-1)
		return -1;

	src->shm_ptr = shm;
	if (stride == req->src_w * 4) {
		src->pixels = (const uint32_t*)shm;
		return 0;
	}

	src->packed_pixels = (uint32_t*)malloc(expected);
	if (src->packed_pixels == NULL) {
		shmdt(shm);
		src->shm_ptr = NULL;
		return -1;
	}

	for (y = 0; y < req->src_h; y++) {
		memcpy(((uint8_t*)src->packed_pixels) + y * req->src_w * 4,
				shm + y * stride,
				req->src_w * 4);
	}
	src->pixels = src->packed_pixels;
	return 0;
}

static void g2d_blit_source_release(g2d_blit_source_t* src) {
	if (src == NULL)
		return;
	if (src->shm_ptr != NULL)
		shmdt(src->shm_ptr);
	free(src->packed_pixels);
	memset(src, 0, sizeof(*src));
}

static int32_t g2d_handle_get_info(proto_t* ret, g2d_state_t* state) {
	g2d_info_t info;

	if (ret == NULL || state == NULL)
		return -1;

	memset(&info, 0, sizeof(info));
	info.width = state->surface.width;
	info.height = state->surface.height;
	info.depth = state->surface.depth;
	info.format = state->surface.format;
	info.backend = G2D_BACKEND_VC4_KMS_V3D;
	PF->init(ret)->add(ret, &info, sizeof(info));
	return 0;
}

static int32_t g2d_handle_get_pixel(proto_t* in, proto_t* ret, g2d_state_t* state) {
	g2d_pixel_req_t req;
	uint32_t pixel;

	if (in == NULL || ret == NULL || state == NULL)
		return -1;
	if (proto_read_to(in, &req, sizeof(req)) != sizeof(req))
		return -1;
	if (g2d_surface_read_pixel(state, req.x, req.y, &pixel) != 0)
		return -1;
	PF->init(ret)->add(ret, &pixel, sizeof(pixel));
	return 0;
}

static int32_t g2d_handle_get_stats(proto_t* ret, g2d_state_t* state) {
	g2d_stats_t stats;

	if (ret == NULL || state == NULL)
		return -1;

	stats = state->stats;
	stats.backend = G2D_BACKEND_VC4_KMS_V3D;
	stats.vc_ready = 1;
	PF->init(ret)->add(ret, &stats, sizeof(stats));
	return 0;
}

static int32_t g2d_handle_clear(proto_t* in, g2d_state_t* state) {
	uint32_t color;
        uint32_t pixel = 0;

	if (in == NULL || state == NULL)
		return -1;
	if (proto_read_to(in, &color, sizeof(color)) != sizeof(color))
		return -1;
        if (vc4_kms_v3d_clear(&state->vc4, color) == 0 &&
                        vc4_kms_v3d_read_pixel(&state->vc4, 0, 0, &pixel) == 0 &&
                        pixel == color) {
                state->stats.vc_clear_ops++;
                return 0;
        }

        klog("g2dd: hardware clear failed got=%x want=%x\n", pixel, color);
        return -1;
}

static int32_t g2d_handle_fill_rect(proto_t* in, g2d_state_t* state) {
	g2d_fill_req_t req;
        g2d_rect_t clipped;
        uint32_t verify_x;
        uint32_t verify_y;
        uint32_t pixel = 0;
	int32_t ret;

	if (in == NULL || state == NULL)
		return -1;
	if (proto_read_to(in, &req, sizeof(req)) != sizeof(req))
		return -1;
	ret = vc4_kms_v3d_fill_rect(&state->vc4, &req);
        if (ret == 0) {
                clipped = req.rect;
                if (clipped.x < 0) {
                        clipped.w += clipped.x;
                        clipped.x = 0;
                }
                if (clipped.y < 0) {
                        clipped.h += clipped.y;
                        clipped.y = 0;
                }
                if (clipped.x + clipped.w > (int32_t)state->surface.width)
                        clipped.w = (int32_t)state->surface.width - clipped.x;
                if (clipped.y + clipped.h > (int32_t)state->surface.height)
                        clipped.h = (int32_t)state->surface.height - clipped.y;
                if (clipped.w > 0 && clipped.h > 0) {
                        verify_x = (uint32_t)clipped.x;
                        verify_y = (uint32_t)clipped.y;
                        if (vc4_kms_v3d_read_pixel(&state->vc4,
                                        (int32_t)verify_x, (int32_t)verify_y, &pixel) == 0 &&
                                        pixel == req.color) {
                                state->stats.vc_fill_ops++;
                                return 0;
                        }
                        klog("g2dd: hardware fill verify failed rect=(%d,%d %dx%d) got=%x want=%x\n",
                                        clipped.x, clipped.y, clipped.w, clipped.h, pixel, req.color);
                }
                else {
                        state->stats.vc_fill_ops++;
                        return 0;
                }
        }

        klog("g2dd: hardware fill failed ret=%d rect=(%d,%d %dx%d) color=%x\n",
                        ret, req.rect.x, req.rect.y, req.rect.w, req.rect.h, req.color);
        return ret != 0 ? ret : -1;
}

static int32_t g2d_handle_blit(proto_t* in, g2d_state_t* state, uint8_t use_alpha) {
	g2d_blit_req_t req;
	g2d_blit_source_t src;
	int32_t ret;

	if (in == NULL || state == NULL)
		return -1;
	if (proto_read_to(in, &req, sizeof(req)) != sizeof(req))
		return -1;
	if (req.src_format != G2D_FMT_ARGB8888 || req.src_w == 0 || req.src_h == 0)
		return -1;
	if (g2d_blit_source_attach(&req, &src) != 0)
		return -1;

	ret = vc4_kms_v3d_blit(&state->vc4, &req, src.pixels, use_alpha);
        g2d_blit_source_release(&src);
        if (ret != 0) {
                klog("g2dd: hardware blit failed ret=%d alpha=%u src=%ux%u dst=(%d,%d %dx%d) rot=%u\n",
                        ret, use_alpha, req.src_w, req.src_h,
                        req.dx, req.dy, req.dw, req.dh, req.rotate);
                return ret;
        }
        if (use_alpha != 0)
                state->stats.vc_alpha_blit_ops++;
        else
                state->stats.vc_blit_ops++;
        return 0;
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
	g2d_state_t* state = (g2d_state_t*)p;

	(void)dev;
	(void)from_pid;
	if (argc <= 0 || argv == NULL || argv[0] == NULL || state == NULL)
		return NULL;

	if (strcmp(argv[0], "info") == 0) {
		static char info[96];
		snprintf(info, sizeof(info), "%ux%u argb8888 offscreen via vc4_kms_v3d",
				state->surface.width, state->surface.height);
		return g2d_strdup(info);
	}
	if (strcmp(argv[0], "clear") == 0 && argc > 1) {
		uint32_t color = (uint32_t)strtoul(argv[1], NULL, 0);
		if (vc4_kms_v3d_clear(&state->vc4, color) == 0)
			state->stats.vc_clear_ops++;
		return g2d_strdup("ok");
	}
	return NULL;
}

static int g2d_dev_cntl(vdevice_t* dev, int from_pid, int cmd, proto_t* in, proto_t* ret, void* p) {
	g2d_state_t* state = (g2d_state_t*)p;

	(void)dev;
	(void)from_pid;
	if (state == NULL)
		return -1;

	switch (cmd) {
	case G2D_DEV_CNTL_GET_INFO:
		return g2d_handle_get_info(ret, state);
	case G2D_DEV_CNTL_CLEAR:
		return g2d_handle_clear(in, state);
	case G2D_DEV_CNTL_FILL_RECT:
		return g2d_handle_fill_rect(in, state);
	case G2D_DEV_CNTL_BLIT:
		return g2d_handle_blit(in, state, 0);
	case G2D_DEV_CNTL_BLIT_ALPHA:
		return g2d_handle_blit(in, state, 1);
	case G2D_DEV_CNTL_PRESENT:
		return -1;
	case G2D_DEV_CNTL_GET_PIXEL:
		return g2d_handle_get_pixel(in, ret, state);
	case G2D_DEV_CNTL_GET_STATS:
		return g2d_handle_get_stats(ret, state);
	default:
		return -1;
	}
}

static void read_config(const char* conf_file, uint32_t* w, uint32_t* h, uint32_t* dep) {
	json_var_t* conf_var;

	if (conf_file == NULL || conf_file[0] == 0)
		conf_file = "/etc/display.json";
	conf_var = json_parse_file(conf_file);

	/* The offscreen surface still reuses the display config geometry by default. */
	*w = (uint32_t)json_get_int_def(conf_var, "width", 1024);
	*h = (uint32_t)json_get_int_def(conf_var, "height", 768);
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
	const char* conf_file = "/etc/display.json";
	g2d_state_t state;
	vdevice_t dev;
	uint32_t w = 0;
	uint32_t h = 0;
	uint32_t dep = 32;
	int opti;
	const char* mnt_point;

	opti = doargs(argc, argv, &conf_file);
	mnt_point = (opti < argc && opti >= 0) ? argv[opti] : "/dev/g2d";
	read_config(conf_file, &w, &h, &dep);

	memset(&state, 0, sizeof(state));
	if (g2d_state_setup(&state, w, h, dep) != 0) {
		klog("g2dd: failed to start, vc4_kms_v3d backend is required\n");
		return -1;
	}

	memset(&dev, 0, sizeof(dev));
	strcpy(dev.name, "g2d");
	dev.dev_cntl = g2d_dev_cntl;
	dev.cmd = g2d_cmd;
	dev.extra_data = &state;

	if (device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666) != 0) {
		g2d_state_teardown(&state);
		return -1;
	}

	g2d_state_teardown(&state);
	return 0;
}
