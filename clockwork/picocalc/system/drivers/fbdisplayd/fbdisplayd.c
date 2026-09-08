#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <displayd/displayd.h>
#include <graph/graph.h>
#include <graph/graph_png.h>
#include <bsp/bsp_fb.h>

#include "ili9488.h"

static uint32_t flush(const disp_info_t* fbinfo, const graph_t* g) {
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
static uint32_t flush_rect(const disp_info_t* fbinfo, const graph_t* g, const grect_t* r) {
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

static disp_info_t* get_info(void) {
    return bsp_get_fbinfo();
}

static int32_t init(uint32_t w, uint32_t h, uint32_t dep) {
    bsp_fb_init(w, h, dep);
    ili9488_init();
    ili9488_clear(0);
    return 0;
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
    displayd_t fbdisplayd;
    memset(&fbdisplayd, 0, sizeof(fbdisplayd));
        int opti = doargs(argc, argv);
        const char* mnt_point = (opti < argc && opti >= 0) ? argv[opti]: "/dev/disp0";

    fbdisplayd.splash = NULL;
    fbdisplayd.flush = flush;
    fbdisplayd.init = init;
    fbdisplayd.get_info = get_info;
    fbdisplayd_set_flush_rect(flush_rect);

        return fbdisplayd_run(&fbdisplayd, mnt_point, LCD_WIDTH, LCD_HEIGHT, _conf_file, _display_index);
}
