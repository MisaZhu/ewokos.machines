#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <displayd/displayd.h>
#include <graph/graph.h>
#include <graph/graph_png.h>
#include <bsp/bsp_fb.h>
#include <arch/bcm2712/rp1_dpi.h>
#include <tinyjson/tinyjson.h>
#include <ewoksys/klog.h>

static graph_t* _g = NULL;
static const char* _conf_file = "/etc/display.json";
static int _dpi_output = 0;
static volatile int _dpi_ok = 0;
static bcm2712_dpi_timing_t _dpi_timing;

static void blt16(uint32_t* src, uint16_t* dst, uint32_t w, uint32_t h) {
    uint32_t sz = w * h;
    uint32_t i;

    for (i = 0; i < sz; i++) {
        uint32_t s = src[i];
        uint8_t r = (s >> 16) & 0xff;
        uint8_t g = (s >> 8)  & 0xff;
        uint8_t b = s & 0xff;
        dst[i] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    }
}

static uint32_t blt32(const disp_info_t* fbinfo, const graph_t* g) {
    uint32_t bytes_per_pixel = fbinfo->depth / 8;
    uint8_t* dst = (uint8_t*)(uintptr_t)(fbinfo->pointer +
            fbinfo->yoffset * fbinfo->pitch +
            fbinfo->xoffset * bytes_per_pixel);
    uint32_t total_bytes = (uint32_t)g->h * (uint32_t)g->w * bytes_per_pixel;

    /* wrap the framebuffer as a graph (row stride = pitch) and let
       graph_blt run the arch-accelerated 1:1 copy */
    graph_t fb_g;
    graph_init(&fb_g, (const uint32_t*)dst,
            (int32_t)(fbinfo->pitch / bytes_per_pixel),
            (int32_t)fbinfo->yoffset + g->h);
    graph_blt((graph_t*)g, 0, 0, g->w, g->h,
            &fb_g, 0, 0, g->w, g->h);
    return total_bytes;
}

static uint32_t blt16_pitch(const disp_info_t* fbinfo, const graph_t* g) {
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

static uint32_t flush(const disp_info_t* fbinfo, const graph_t* g) {
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

static disp_info_t* get_info(void) {
    return bsp_get_fbinfo();
}

static int32_t init(uint32_t w, uint32_t h, uint32_t dep) {
    if (_dpi_output) {
        /* explicit opt-in via "output":"dpi"; fall back to HDMI on failure */
        if (bsp_fb_init_dpi(w, h, dep, &_dpi_timing) == 0) {
            _dpi_ok = 1;
            return 0;
        }
        slog("fbdisplayd: dpi init failed, falling back to hdmi\n");
    }
    return bsp_fb_init(w, h, dep);
}

/*
 * Watchdog thread: independent of any GUI redraws, polls the DPI scanout
 * engine once per second. bcm2712_rp1_dpi_check() logs a status snapshot
 * and restarts the engine if it ever stops.
 */
static void* dpi_watchdog(void* arg) {
    (void)arg;
    while (!_dpi_ok)
        usleep(100000);
    while (1) {
        sleep(1);
        bcm2712_rp1_dpi_check();
    }
    return NULL;
}

/*
 * DPI specific keys in the framebuffer config, e.g.:
 *   "output":"dpi", "pclk":33000000,
 *   "hfp":16, "hsync":30, "hbp":80,
 *   "vfp":4,  "vsync":4,  "vbp":14,
 *   "hsync_pol":1, "vsync_pol":1,
 *   "mode":7          (7 = 24-bit DPI888, 6 = DPI666 panels)
 * Without pclk (or with pclk 0) CVT-RB 60Hz timings are derived.
 */
static void load_dpi_conf(const char* conf_file) {
    json_var_t *conf_var = json_parse_file(conf_file);
    if (conf_var == NULL)
        return;

    const char* output = json_get_str_def(conf_var, "output", "");
    if (strcmp(output, "dpi") != 0) {
        json_var_unref(conf_var);
        return;
    }

    memset(&_dpi_timing, 0, sizeof(_dpi_timing));
    _dpi_timing.pixel_clock_hz = (uint32_t)json_get_int_def(conf_var, "pclk", 0);
    _dpi_timing.hfp    = (uint32_t)json_get_int_def(conf_var, "hfp", 0);
    _dpi_timing.hsync  = (uint32_t)json_get_int_def(conf_var, "hsync", 0);
    _dpi_timing.hbp    = (uint32_t)json_get_int_def(conf_var, "hbp", 0);
    _dpi_timing.vfp    = (uint32_t)json_get_int_def(conf_var, "vfp", 0);
    _dpi_timing.vsync  = (uint32_t)json_get_int_def(conf_var, "vsync", 0);
    _dpi_timing.vbp    = (uint32_t)json_get_int_def(conf_var, "vbp", 0);
    _dpi_timing.hsync_pos = (uint8_t)json_get_int_def(conf_var, "hsync_pol", 1);
    _dpi_timing.vsync_pos = (uint8_t)json_get_int_def(conf_var, "vsync_pol", 1);
    _dpi_timing.mode = (uint8_t)json_get_int_def(conf_var, "mode", 7);
    _dpi_timing.bl_pin = (int8_t)json_get_int_def(conf_var, "bl_pin", -1);
    json_var_unref(conf_var);
    _dpi_output = 1;
    slog("fbdisplayd: dpi output selected mode=%u pclk=%u bl_pin=%d\n",
            _dpi_timing.mode, _dpi_timing.pixel_clock_hz, _dpi_timing.bl_pin);
}

static int _display_index = 0;

static int doargs(int argc, char* argv[]) {
    int c = 0;
    while (c != -1) {
                c = getopt (argc, argv, "c:i:");
        if(c == -1)
            break;

        switch (c) {
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
    _g = NULL;
    memset(&fbdisplayd, 0, sizeof(fbdisplayd));

    int opti = doargs(argc, argv);
    const char* mnt_point = (opti < argc && opti >= 0) ? argv[opti]: "/dev/disp0";

    load_dpi_conf(_conf_file);
    if (_dpi_output) {
        pthread_t th;
        pthread_create(&th, NULL, dpi_watchdog, NULL);
    }

    fbdisplayd.splash = NULL;
    fbdisplayd.flush = flush;
    fbdisplayd.init = init;
    fbdisplayd.get_info = get_info;
    fbdisplayd_set_flush_rect(fbdisplayd_flush_rect_to);
        int res = fbdisplayd_run(&fbdisplayd, mnt_point, 0, 0, _conf_file, _display_index);
    if(_g != NULL)
        graph_free(_g);
    return res;
}
