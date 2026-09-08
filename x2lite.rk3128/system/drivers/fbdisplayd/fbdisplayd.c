#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <displayd/displayd.h>
#include <graph/graph.h>
#include <graph/graph_png.h>
#include <bsp/bsp_fb.h>

int argv2rgb(uint8_t  *out,  uint32_t *in , int w, int h)
{
    for(int i = 0; i < w*h; i++){
         register uint32_t color = *in++;
         *out++=color ;
         *out++=color >> 8;
         *out++=color >> 16;
    }	

    return 0;
}


static uint32_t flush(const disp_info_t* fbinfo, const graph_t* g) {
    uint32_t sz = 4 * g->w * g->h;
    memcpy((uint8_t*)fbinfo->pointer, (uint32_t*)g->buffer, sz);
    return sz;
}

static disp_info_t* get_info(void) {
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
    displayd_t fbdisplayd;
    memset(&fbdisplayd, 0, sizeof(fbdisplayd));
        int opti = doargs(argc, argv);
        const char* mnt_point = (opti < argc && opti >= 0) ? argv[opti]: "/dev/disp0";

    fbdisplayd.splash = NULL;
    fbdisplayd.flush = flush;
    fbdisplayd.init = init;
    fbdisplayd.get_info = get_info;
    /*linear 32bpp scan-out: the generic per-rect copy pushes only the
      compositor's dirty rects instead of the whole frame*/
    fbdisplayd_set_flush_rect(fbdisplayd_flush_rect_to);

        return fbdisplayd_run(&fbdisplayd, mnt_point, 640, 480, _conf_file, _display_index);
}
