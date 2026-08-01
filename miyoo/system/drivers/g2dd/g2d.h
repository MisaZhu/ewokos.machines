#ifndef MIYOO_G2D_H
#define MIYOO_G2D_H

#include <stdint.h>

enum {
	G2D_DEV_CNTL_GET_INFO = 0,
	G2D_DEV_CNTL_CLEAR,
	G2D_DEV_CNTL_FILL_RECT,
	G2D_DEV_CNTL_BLIT,
	G2D_DEV_CNTL_BLIT_ALPHA,
	G2D_DEV_CNTL_PRESENT
};

enum {
	G2D_FMT_ARGB8888 = 0
};

enum {
	G2D_BACKEND_SOFT_NV12 = 0,
	G2D_BACKEND_MI_GFX = 1,
	G2D_BACKEND_SSD20XD_GE = 2
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
	uint8_t reserved[7];
} g2d_blit_req_t;

#endif
