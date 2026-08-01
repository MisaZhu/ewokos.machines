#ifndef RASPIX_VC_G2D_H
#define RASPIX_VC_G2D_H

#include <stdint.h>

/*
 * VideoCore-accessible buffer: physically contiguous, mapped non-cached on
 * the CPU side, so no cache maintenance is needed between ARM and VC.
 * "bus" is the VC bus address (ARM physical + non-cached alias).
 */
typedef struct {
	uint32_t vaddr; /* CPU virtual address (non-cached mapping), 0 if none */
	uint32_t bus;   /* VideoCore bus address */
	uint32_t size;  /* allocation size in bytes, 0 for foreign buffers */
} vc_g2d_buf_t;

enum {
	VC_G2D_BLIT_OPAQUE = 0x00000000u, /* raw copy, source alpha ignored */
	VC_G2D_BLIT_FILL   = 0x00000001u, /* src bus field carries the fill color */
	VC_G2D_BLIT_ALPHA  = 0x01000001u  /* per-pixel SRC_OVER blend */
};

int  vc_g2d_init(void);

/* allocate/free a VC-accessible buffer from the kernel DMA block */
int  vc_g2d_buf_alloc(vc_g2d_buf_t* buf, uint32_t size);
void vc_g2d_buf_free(vc_g2d_buf_t* buf);
/* wrap an existing framebuffer mapping (no ownership) */
void vc_g2d_buf_wrap(vc_g2d_buf_t* buf, uint32_t vaddr, uint32_t phy_base, uint32_t bus_base);

/* scaled blit: src rect (sx,sy,sw,sh) -> dst rect (dx,dy,dw,dh),
 * dw/dh may differ from sw/sh (hardware scale) */
int  vc_g2d_blit(const vc_g2d_buf_t* src,
		uint32_t src_w, uint32_t src_h, uint32_t src_pitch,
		int32_t sx, int32_t sy, int32_t sw, int32_t sh,
		const vc_g2d_buf_t* dst,
		uint32_t dst_w, uint32_t dst_h, uint32_t dst_pitch,
		int32_t dx, int32_t dy, int32_t dw, int32_t dh,
		uint32_t flags);

/* solid fill of a sub-rect inside dst */
int  vc_g2d_fill(const vc_g2d_buf_t* dst,
		uint32_t dst_w, uint32_t dst_h, uint32_t dst_pitch,
		int32_t x, int32_t y, int32_t w, int32_t h,
		uint32_t color);

#endif
