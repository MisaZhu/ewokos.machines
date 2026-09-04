#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <displayd/displayd.h>
#include <graph/graph.h>
#include <graph/graph_png.h>
#include <bsp/bsp_fb.h>
#include <ewoksys/mmio.h>
#include "st7586.h"

static void argb32_to_gray(uint32_t *argb, uint8_t *gray, int size){
    uint8_t *p = (uint8_t*)argb;
    for(int i = 0; i < size; i++){
        uint8_t b = *p++;
        uint8_t g = *p++;
        uint8_t r = *p++;
        p++;
        gray[i] = (r+g+b)/3;
    }
}

static uint32_t flush(const disp_info_t* fbinfo, const graph_t* g) {
    argb32_to_gray(g->buffer, (uint8_t*)fbinfo->pointer, g->w * g->h);
    st7586_update((uint8_t*)fbinfo->pointer, g->w , g->h);
    return 4 * g->w * g->h;
}

static disp_info_t* get_info(void) {
    return bsp_get_fbinfo();
}

static int32_t init(uint32_t w, uint32_t h, uint32_t dep) {
    return bsp_fb_init(w, h, dep);
}

static void splash(graph_t* g, const char* logo_fname) {
    graph_clear(g, 0xffffffff);
    graph_t* logo = png_image_new(logo_fname);
    if(logo != NULL) {
        graph_blt_alpha(logo, 0, 0, logo->w, logo->h,
                g, (g->w-logo->w)/2, (g->h-logo->h)/2, logo->w, logo->h, 0xff);
        graph_free(logo);
    }
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
    _mmio_base = mmio_map();
    st7586_init();

    fbdisplayd.splash = splash;
    fbdisplayd.flush = flush;
    fbdisplayd.init = init;
    fbdisplayd.get_info = get_info;
    return fbdisplayd_run(&fbdisplayd, mnt_point, 178, 128, _conf_file, _display_index);
}
