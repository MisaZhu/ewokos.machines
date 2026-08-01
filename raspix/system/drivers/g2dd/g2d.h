#ifndef RASPIX_G2D_H
#define RASPIX_G2D_H

#include <stdint.h>

enum {
	G2D_DEV_CNTL_GET_INFO = 0,
	G2D_DEV_CNTL_CLEAR,
	G2D_DEV_CNTL_FILL_RECT,
	G2D_DEV_CNTL_BLIT,
	G2D_DEV_CNTL_BLIT_ALPHA,
	G2D_DEV_CNTL_PRESENT,
	G2D_DEV_CNTL_GET_PIXEL,
	G2D_DEV_CNTL_GET_STATS
};

enum {
	G2D_FMT_ARGB8888 = 0
};

enum {
	G2D_ROTATE_0 = 0,
	G2D_ROTATE_90 = 1,
	G2D_ROTATE_180 = 2,
	G2D_ROTATE_270 = 3
};

enum {
	G2D_BACKEND_SOFT_NV12 = 0,
	G2D_BACKEND_MI_GFX = 1,
	G2D_BACKEND_VC_MAILBOX = 2,
	G2D_BACKEND_VC4_KMS_V3D = 3
};

typedef struct {
	int32_t x;
	int32_t y;
	int32_t w;
	int32_t h;
} g2d_rect_t;

typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t depth;
	uint32_t format;
	uint32_t backend;
} g2d_info_t;

typedef struct {
	g2d_rect_t rect;
	uint32_t color;
} g2d_fill_req_t;

typedef struct {
	int32_t x;
	int32_t y;
} g2d_pixel_req_t;

typedef struct {
	uint32_t src_w;
	uint32_t src_h;
	uint32_t src_stride;
	uint32_t src_format;
	int32_t src_shm_id;
	uint32_t src_size;
	int32_t sx;
	int32_t sy;
	int32_t sw;
	int32_t sh;
	int32_t dx;
	int32_t dy;
	int32_t dw;
	int32_t dh;
	uint8_t alpha;
	uint8_t rotate;
	uint8_t reserved[6];
} g2d_blit_req_t;

typedef struct {
	uint32_t backend;
	uint32_t vc_ready;
	uint32_t vc_clear_ops;
	uint32_t vc_fill_ops;
	uint32_t vc_blit_ops;
	uint32_t vc_alpha_blit_ops;
	uint32_t vc_present_ops;
	uint32_t soft_clear_ops;
	uint32_t soft_fill_ops;
	uint32_t soft_blit_ops;
	uint32_t soft_alpha_blit_ops;
	uint32_t soft_present_ops;
	uint32_t soft_fallback_ops;
} g2d_stats_t;

#endif
