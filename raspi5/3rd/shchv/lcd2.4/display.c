#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdbool.h>
#include <bsp/bsp_spi.h>
#include <displayd/displayd.h>
#include <ili9341/ili9341.h>
#include <xpt2046/xpt2046.h>
#include <xpt2046/tp_filter.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/vfs.h>
#include <ewoksys/proc.h>

static int _lcd_dc_pin = 22;
static int _lcd_cs_pin = 8;
static int _lcd_rst_pin = 27;
static int _lcd_bl_pin = -1;
static int _tp_cs_pin = 7;
static int _tp_irq_pin = 17;
static int _tp_spi_div = 64;
static int _tp_spi_select = SPI_SELECT_1;

int  do_flush(const void* buf, uint32_t size) {
    ili9341_flush(buf, size);
    return 0;
}

static void show_test_pattern(uint32_t w, uint32_t h) {
    static uint32_t buf[320 * 240];

    for(uint32_t y = 0; y < h; y++) {
        for(uint32_t x = 0; x < w; x++) {
            uint32_t c;
            if(x < (w / 3)) {
                c = 0x00ff0000;
            }
            else if(x < (w * 2 / 3)) {
                c = 0x0000ff00;
            }
            else {
                c = 0x000000ff;
            }

            if(x < 4 || y < 4 || x >= (w - 4) || y >= (h - 4)) {
                c = 0x00ffffff;
            }
            buf[y * w + x] = c;
        }
    }

    do_flush(buf, w * h * 4);
}

void lcd_init(uint32_t w, uint32_t h, uint32_t div) {
    /* SHCHV 2.4" follows the goodtft tft9341 overlay wiring. */
    ili9341_init(w, h, G_ROTATE_270, 0,
        _lcd_dc_pin, _lcd_cs_pin, _lcd_rst_pin, _lcd_bl_pin, div);
}

static uint32_t flush(const disp_info_t* fbinfo, const graph_t* g) {
    uint32_t sz = 4 * g->w * g->h;
    do_flush(g->buffer, sz);
    return sz;
}

static disp_info_t* get_info(void) {
    static disp_info_t fbinfo;
    memset(&fbinfo, 0, sizeof(disp_info_t));
    fbinfo.width = LCD_WIDTH;
    fbinfo.height = LCD_HEIGHT;
    fbinfo.depth = 32;
    return &fbinfo;
}

static int32_t init(uint32_t w, uint32_t h, uint32_t dep) {
    (void)w;
    (void)h;
    (void)dep;
    return 0;
}

static tp_filter_t _fx, _fy;
static bool _pen_down = false;

/*
 * Event queue between the sampler loop (displayd_t.step_loop) and readers:
 * fixed 6-byte events (state, x, y), same wire format as before. A wedged
 * reader drops the oldest event; a blocked reader wakes on the first
 * queued one.
 */
#define TOUCH_CACHE_SIZE 32

static uint16_t touch_data[TOUCH_CACHE_SIZE][3];
static uint32_t touch_data_read = 0;
static uint32_t touch_data_write = 0;

static bool touch_has_data(void) {
    return (touch_data_write - touch_data_read) > 0;
}

static void touch_push(uint16_t state, uint16_t x, uint16_t y) {
    if(touch_data_write - touch_data_read >= TOUCH_CACHE_SIZE)
        touch_data_read++; /* queue full: drop the oldest event */

    uint16_t* evt = touch_data[touch_data_write % TOUCH_CACHE_SIZE];
    evt[0] = state;
    evt[1] = x;
    evt[2] = y;
    touch_data_write++;
}

/* sampling cadence: 10ms -> at most 100 events/s while pressed,
   one plain GPIO read per tick while idle */
#define TP_POLL_US 10000

static const char* _tp_dev_name = "/dev/disp0";

static int tp_read(uint8_t* buf, uint32_t size) {
    if(!touch_has_data())
        return VFS_ERR_RETRY;
    if(size < 6)
        return -1;

    memcpy(buf, touch_data[touch_data_read % TOUCH_CACHE_SIZE], 6);
    touch_data_read++;
    return 6;
}

static int32_t tp_check_poll(void) {
    return touch_has_data() ? VFS_EVT_RD : 0;
}

/*
 * Device-loop tick: sample the touch panel on the shared SPI bus, filter it
 * (clamp + median + moving average, see tp_filter), queue state changes and
 * wake up any blocked reader. The LCD CS is parked deselected around the
 * sample so the panel and the touch controller do not fight over SPI0.
 */
static int tp_step(void) {
    static ewokos_addr_t node = 0;
    uint16_t press, x, y;

    if(_lcd_cs_pin >= 0)
        bsp_gpio_write(_lcd_cs_pin, 1);
    int res = xpt2046_read(&press, &x, &y);
    if(_lcd_cs_pin >= 0)
        bsp_gpio_write(_lcd_cs_pin, 0);

    if(res == 0) {
        if(press != 0) {
            if(!_pen_down) {
                tp_filter_reset(&_fx);
                tp_filter_reset(&_fy);
                _pen_down = true;
                touch_push(1, tp_filter_in(&_fx, x), tp_filter_in(&_fy, y));
            }
            else {
                uint16_t px = _fx.out, py = _fy.out;
                uint16_t fx = tp_filter_in(&_fx, x);
                uint16_t fy = tp_filter_in(&_fy, y);
                if(fx != px || fy != py)
                    touch_push(1, fx, fy);
            }
        }
        else {
            _pen_down = false;
            tp_filter_reset(&_fx);
            tp_filter_reset(&_fy);
            touch_push(0, _fx.out, _fy.out);
        }
    }

    if(touch_has_data()) {
        if(node == 0) {
            fsinfo_t info;
            if(vfs_get_by_name(_tp_dev_name, &info) == 0)
                node = info.node;
        }
        if(node != 0)
            vfs_wakeup(node, VFS_EVT_RD);
    }

    proc_usleep(TP_POLL_US);
    return 0;
}

static int _spi_div = 64;
const char* _conf_file = "";
int _display_index = 0;
static int doargs(int argc, char* argv[]) {
    int c = 0;
    while (c != -1) {
        c = getopt (argc, argv, "c:d:i:");
        if(c == -1)
            break;

        switch (c) {
        case 'd':
            _spi_div = atoi(optarg);
            break;
        case 'i':
            _display_index = atoi(optarg);
            break;
        case 'c':
            _conf_file = optarg;
            break;
        default:
            c = -1;
            break;
        }
    }
    return optind;
}

int main(int argc, char** argv) {
    _spi_div = 4;
    LCD_HEIGHT = 240;
    LCD_WIDTH = 320;

    int opti = doargs(argc, argv);
    const char* mnt_point = (opti < argc && opti >= 0) ? argv[opti]: "/dev/disp0";

    lcd_init(LCD_WIDTH, LCD_HEIGHT, _spi_div);
    xpt2046_set_config(_tp_cs_pin, _tp_irq_pin, _tp_spi_div, _tp_spi_select);
    xpt2046_init(_tp_cs_pin, _tp_irq_pin, _tp_spi_div);
    _tp_dev_name = mnt_point;

    displayd_t display;
    memset(&display, 0, sizeof(displayd_t));
    display.splash = NULL;
    display.flush = flush;
    display.init = init;
    display.read = tp_read;
    display.get_info = get_info;
    display.step_loop = tp_step;
    display.check_poll_events = tp_check_poll;
    fbdisplayd_set_flush_rect(ili9341_flush_rect);
    int ret = fbdisplayd_run(&display, mnt_point, LCD_WIDTH, LCD_HEIGHT, _conf_file, _display_index);
    return ret;
}
