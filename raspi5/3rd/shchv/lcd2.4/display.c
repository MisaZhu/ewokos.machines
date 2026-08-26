#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <bsp/bsp_spi.h>
#include <displayd/displayd.h>
#include <ili9341/ili9341.h>
#include <xpt2046/xpt2046.h>

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

static int tp_read(uint8_t* buf, uint32_t size) {
    memset(buf, 0, size);
    if(size >= 6) {
        uint16_t* d = (uint16_t*)buf;
        if(_lcd_cs_pin >= 0)
            bsp_gpio_write(_lcd_cs_pin, 1);
        xpt2046_read(&d[0], &d[1], &d[2]);
        if(_lcd_cs_pin >= 0)
            bsp_gpio_write(_lcd_cs_pin, 0);
    }
    return 6;
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

    fbdisplayd_t display;
    memset(&display, 0, sizeof(fbdisplayd_t));
    display.splash = NULL;
    display.flush = flush;
    display.init = init;
    display.read = tp_read;
    display.get_info = get_info;
    fbdisplayd_set_flush_rect(ili9341_flush_rect);
    int ret = fbdisplayd_run(&display, mnt_point, LCD_WIDTH, LCD_HEIGHT, _conf_file, _display_index);
    return ret;
}
