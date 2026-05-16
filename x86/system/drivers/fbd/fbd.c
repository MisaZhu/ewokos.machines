#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <fbd/fbd.h>
#include <graph/graph.h>
#include <graph/graph_png.h>
#include <bsp/bsp_fb.h>

static void blt16(uint32_t* src, uint16_t* dst, uint32_t w, uint32_t h) {
	uint32_t sz = w * h;

	for (uint32_t i = 0; i < sz; ++i) {
		uint32_t s = src[i];
		uint8_t r = (s >> 16) & 0xff;
		uint8_t g = (s >> 8) & 0xff;
		uint8_t b = s & 0xff;
		dst[i] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
	}
}

static uint32_t blt32(const fbinfo_t* fbinfo, const graph_t* g) {
	uint32_t bytes_per_pixel = fbinfo->depth / 8;
	uint8_t* dst = (uint8_t*)(uintptr_t)(fbinfo->pointer +
			fbinfo->yoffset * fbinfo->pitch +
			fbinfo->xoffset * bytes_per_pixel);
	const uint32_t* src = g->buffer;
	uint32_t row_bytes = (uint32_t)g->w * bytes_per_pixel;
	uint32_t total_bytes = (uint32_t)g->h * row_bytes;

	if (fbinfo->pitch == row_bytes) {
		memcpy(dst, src, total_bytes);
		return total_bytes;
	}

	for (int32_t y = 0; y < g->h; ++y) {
		uint8_t* dst_row = dst + y * fbinfo->pitch;
		const uint8_t* src_row = (const uint8_t*)(src + y * g->w);
		memcpy(dst_row, src_row, row_bytes);
	}
	return total_bytes;
}

static uint32_t blt16_pitch(const fbinfo_t* fbinfo, const graph_t* g) {
	uint8_t* dst_base = (uint8_t*)(uintptr_t)fbinfo->pointer +
			fbinfo->yoffset * fbinfo->pitch +
			fbinfo->xoffset * 2;
	const uint32_t* src = g->buffer;

	for (int32_t y = 0; y < g->h; ++y) {
		blt16((uint32_t*)(src + y * g->w),
				(uint16_t*)(dst_base + y * fbinfo->pitch),
				g->w, 1);
	}
	return g->w * g->h * 2;
}

static uint32_t flush(const fbinfo_t* fbinfo, const graph_t* g) {
	if (fbinfo->depth != 32 && fbinfo->depth != 16) {
		return 0;
	}

	if (fbinfo->depth == 16) {
		return blt16_pitch(fbinfo, g);
	}
	if ((uintptr_t)fbinfo->pointer != (uintptr_t)g->buffer) {
		return blt32(fbinfo, g);
	}
	return g->w * g->h * 4;
}

static fbinfo_t* get_info(void) {
	return bsp_get_fbinfo();
}

static int32_t init(uint32_t w, uint32_t h, uint32_t dep) {
	return bsp_fb_init(w, h, dep);
}

int main(int argc, char** argv) {
	fbd_t fbd;
	const char* mnt_point = argc > 1 ? argv[1] : "/dev/fb0";

	fbd.splash = NULL;
	fbd.flush = flush;
	fbd.init = init;
	fbd.get_info = get_info;

	return fbd_run(&fbd, mnt_point, 1024, 768, "");
}
