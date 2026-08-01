#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fbd/fbd.h>
#include <graph/graph.h>
#include <graph/graph_png.h>
#include <bsp/bsp_fb.h>

#include "ili9488.h"

static uint32_t flush(const fbinfo_t* fbinfo, const graph_t* g) {
	uint32_t sz = 4 * g->w * g->h;
	uint32_t w = g->w;
	uint32_t h = g->h;

	if (fbinfo != NULL) {
		w = fbinfo->width;
		h = fbinfo->height;
	}

	ili9488_draw(0, 0, (int)w, (int)h, (uint32_t*)g->buffer);
	return sz;
}

/*only convert/scan the rects the compositor declared dirty. g is the full
  frame (stride g->w); ili9488_draw_stride pulls the sub-rect straight out of
  it, then the panel driver's own shadow-diff pushes just the changed pixels
  over SPI. Returns 0 to fall back to a full frame if geometry is unexpected.*/
static uint32_t flush_rect(const fbinfo_t* fbinfo, const graph_t* g, const grect_t* r) {
	(void)fbinfo;
	if (g == NULL || g->buffer == NULL || r == NULL)
		return 0;
	if (g->w != LCD_WIDTH) //panel dest stride is hard-wired to LCD_WIDTH
		return 0;

	int x0 = r->x < 0 ? 0 : r->x;
	int y0 = r->y < 0 ? 0 : r->y;
	int x1 = r->x + r->w; if (x1 > (int)g->w) x1 = g->w;
	int y1 = r->y + r->h; if (y1 > (int)g->h) y1 = g->h;
	if (x0 >= x1 || y0 >= y1)
		return 0;

	int w = x1 - x0;
	int h = y1 - y0;
	uint32_t* buf = (uint32_t*)g->buffer + (uint32_t)y0 * g->w + (uint32_t)x0;
	ili9488_draw_stride(x0, y0, w, h, buf, (int)g->w);
	return (uint32_t)w * (uint32_t)h * 2;
}

static fbinfo_t* get_info(void) {
	return bsp_get_fbinfo();
}

static int32_t init(uint32_t w, uint32_t h, uint32_t dep) {
	bsp_fb_init(w, h, dep);
	ili9488_init();
	ili9488_clear(0);
	return 0;
}

int main(int argc, char** argv) {
	fbd_t fbd;
 	memset(&fbd, 0, sizeof(fbd));
	const char* mnt_point = argc > 1 ? argv[1]: "/dev/fb0";

	fbd.splash = NULL;
	fbd.flush = flush;
	fbd.init = init;
	fbd.get_info = get_info;
	fbd_set_flush_rect(flush_rect);

	return fbd_run(&fbd, mnt_point, LCD_WIDTH, LCD_HEIGHT, "");
}
