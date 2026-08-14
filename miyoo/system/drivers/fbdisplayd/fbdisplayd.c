#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <displayd/displayd.h>
#include <graph/graph.h>
#include <graph/graph_png.h>
#include <graph/uv12.h>
#include <bsp/bsp_fb.h>


static uint32_t flush(const fbinfo_t* fbinfo, const graph_t* g) {
	rgb2nv12(fbinfo->pointer, g->buffer, g->w, g->h);
	return 4 * g->w * g->h;
}

/*same integer coefficients as rgb2nv12 (graph/uv12.c); pixels are BGRA*/
static inline uint8_t px2y(uint32_t p) {
	uint32_t b = p & 0xff;
	uint32_t g = (p >> 8) & 0xff;
	uint32_t r = (p >> 16) & 0xff;
	return (uint8_t)((306 * r + 601 * g + 117 * b) >> 10);
}

static inline void px2uv(uint32_t p, uint8_t* uv) {
	int32_t b = p & 0xff;
	int32_t g = (p >> 8) & 0xff;
	int32_t r = (p >> 16) & 0xff;
	uv[0] = (uint8_t)(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
	uv[1] = (uint8_t)(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
}

/*push only the dirty rect. rgb2nv12 renders the frame rotated 180 degrees
  (output pixel (x,y) reads source (w-1-x, h-1-y)), so the dirty rect is
  mirrored to the matching output region and snapped out to even 2x2 blocks
  for the subsampled chroma. Returns 0 to make libfbdisplayd fall back to a full
  frame whenever the geometry cannot be honoured.*/
static uint32_t flush_rect(const fbinfo_t* fbinfo, const graph_t* g, const grect_t* r) {
	if(fbinfo == NULL || g == NULL || g->buffer == NULL || r == NULL)
		return 0;
	if(fbinfo->pointer == 0)
		return 0;
	int w = g->w;
	int h = g->h;
	if((uint32_t)w != fbinfo->width || (uint32_t)h != fbinfo->height)
		return 0;
	if((w & 1) || (h & 1)) //need even dims for 2x2 chroma blocks
		return 0;
	if(r->w <= 0 || r->h <= 0)
		return 0;

	/*clip the dirty rect to the frame*/
	int sx0 = r->x < 0 ? 0 : r->x;
	int sy0 = r->y < 0 ? 0 : r->y;
	int sx1 = r->x + r->w; if(sx1 > w) sx1 = w;
	int sy1 = r->y + r->h; if(sy1 > h) sy1 = h;
	if(sx0 >= sx1 || sy0 >= sy1)
		return 0;

	/*mirror to the rotated output rect, then snap to even 2x2 blocks*/
	int ox0 = (w - sx1) & ~1;
	int oy0 = (h - sy1) & ~1;
	int ox1 = ((w - sx0) + 1) & ~1;
	int oy1 = ((h - sy0) + 1) & ~1;
	if(ox0 < 0) ox0 = 0;
	if(oy0 < 0) oy0 = 0;
	if(ox1 > w) ox1 = w;
	if(oy1 > h) oy1 = h;

	uint8_t* y_plane = (uint8_t*)(uintptr_t)fbinfo->pointer;
	uint8_t* uv_plane = y_plane + w * h;
	const uint32_t* in = g->buffer;

	for(int oy = oy0; oy < oy1; oy += 2) {
		uint8_t* y_row0 = y_plane + oy * w;
		uint8_t* y_row1 = y_plane + (oy + 1) * w;
		uint8_t* uv_row = uv_plane + (oy >> 1) * w;
		const uint32_t* src_row0 = in + (h - 1 - oy) * w;
		const uint32_t* src_row1 = in + (h - 2 - oy) * w;
		for(int ox = ox0; ox < ox1; ox += 2) {
			uint32_t p00 = src_row0[w - 1 - ox];
			uint32_t p01 = src_row0[w - 2 - ox];
			uint32_t p10 = src_row1[w - 1 - ox];
			uint32_t p11 = src_row1[w - 2 - ox];
			y_row0[ox]     = px2y(p00);
			y_row0[ox + 1] = px2y(p01);
			y_row1[ox]     = px2y(p10);
			y_row1[ox + 1] = px2y(p11);
			px2uv(p00, uv_row + ox);
		}
	}
	return (uint32_t)(ox1 - ox0) * (uint32_t)(oy1 - oy0) * 2;
}

static fbinfo_t* get_info(void) {
	return bsp_get_fbinfo();
}

static int32_t init(uint32_t w, uint32_t h, uint32_t dep) {
	return bsp_fb_init(w, h, dep);
}

static const char* _conf_file = "";
static int _display_index = 0;
static int doargs(int argc, char* argv[]) {
	int c = 0;

	while(c != -1) {
		c = getopt(argc, argv, "c:i:");
		if(c == -1)
			break;

		switch(c) {
		case 'c':
			_conf_file = optarg;
			break;
		case 'i':
			_display_index = atoi(optarg);
			break;
		default:
			c = -1;
			break;
		}
	}
	return optind;
}

int main(int argc, char** argv) {
	fbdisplayd_t fbdisplayd;
  	memset(&fbdisplayd, 0, sizeof(fbdisplayd));
        int opti = doargs(argc, argv);
        const char* mnt_point = (opti < argc && opti >= 0) ? argv[opti]: "/dev/disp0";

	fbdisplayd.splash = NULL;
	fbdisplayd.flush = flush;
	fbdisplayd.init = init;
	fbdisplayd.get_info = get_info;
	fbdisplayd_set_flush_rect(flush_rect);
        return fbdisplayd_run(&fbdisplayd, mnt_point, 640, 480, _conf_file, _display_index);
}
