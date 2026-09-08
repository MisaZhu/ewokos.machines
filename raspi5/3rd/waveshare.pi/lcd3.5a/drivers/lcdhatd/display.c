#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdbool.h>
#include <bsp/bsp_spi.h>
#include <displayd/displayd.h>
#include <ili9486/ili9486.h>
#include <xpt2046/xpt2046.h>
#include <xpt2046/tp_filter.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/vfs.h>
#include <ewoksys/proc.h>

/*
 * Waveshare 3.5A keeps the same 40-pin header wiring on Raspberry Pi 5.
 * The Pi 5 specific part is handled by the raspi5 BSP/RP1 SPI+GPIO backend.
 */
static int _lcd_dc_pin = 24;
static int _lcd_cs_pin = 8;
static int _lcd_rst_pin = 25;
static int _lcd_bl_pin = 18;
static int _lcd_spi_div = 4;
static int _lcd_spi_select = SPI_SELECT_0;
static const int _lcd_inversion = 0;

static int _tp_cs_pin = 7;
static int _tp_irq_pin = 17;
static int _tp_spi_div = 64;
static int _tp_spi_select = SPI_SELECT_1;
static const char* _conf_file = "";
static int _display_index = 0;
static uint16_t _lcd_rot = G_ROTATE_90;
static uint16_t* _lcd35a_flush_buf = NULL;
static uint32_t _lcd35a_flush_pixels = 0;
static uint32_t* _lcd35a_shadow_argb = NULL;
static uint32_t _lcd35a_shadow_pixels = 0;
static uint8_t _lcd35a_shadow_ready = 0;

static inline uint8_t lcd35a_madctl(uint16_t rot) {
    switch(rot) {
    case G_ROTATE_0:
        return 0x88;
    case G_ROTATE_90:
        return 0x28;
    case G_ROTATE_180:
        return 0x48;
    case G_ROTATE_270:
        return 0xE8;
    default:
        return 0x28;
    }
}

static inline void lcd35a_spi_send(uint8_t byte) {
    bsp_spi_transfer(byte);
}

static inline void lcd35a_write_command(uint8_t command) {
    bsp_gpio_write(_lcd_dc_pin, 0);
    if(ILI9486_REG_WIDTH_16)
        lcd35a_spi_send(0x00);
    lcd35a_spi_send(command);
    bsp_gpio_write(_lcd_dc_pin, 1);
}

static inline void lcd35a_write_data(uint8_t data) {
    if(ILI9486_REG_WIDTH_16)
        lcd35a_spi_send(0x00);
    lcd35a_spi_send(data);
}

static inline void lcd35a_start(void) {
    if(_lcd_cs_pin >= 0)
        bsp_gpio_write(_lcd_cs_pin, 0);
    if(_lcd_spi_select >= 0) {
        bsp_spi_select(_lcd_spi_select);
        bsp_spi_activate(1);
    }
}

static inline void lcd35a_end(void) {
    if(_lcd_spi_select >= 0)
        bsp_spi_activate(0);
    if(_lcd_cs_pin >= 0)
        bsp_gpio_write(_lcd_cs_pin, 1);
}

static inline uint16_t lcd35a_argb_to_rgb565(uint32_t pixel) {
    uint16_t r = (pixel >> 19) & 0x1f;
    uint16_t g = (pixel >> 10) & 0x3f;
    uint16_t b = (pixel >> 3) & 0x1f;
    uint16_t rgb565 = (r << 11) | (g << 5) | b;
    return (rgb565 >> 8) | (rgb565 << 8);
}

static void lcd35a_set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    uint16_t x1 = x + w - 1;
    uint16_t y1 = y + h - 1;
    uint8_t reg[8];

    reg[0] = 0x00;
    reg[1] = 0x2A;
    bsp_gpio_write(_lcd_dc_pin, 0);
    bsp_spi_send_recv(reg, NULL, 2);
    bsp_gpio_write(_lcd_dc_pin, 1);

    reg[0] = 0x00;
    reg[1] = (x >> 8) & 0xff;
    reg[2] = 0x00;
    reg[3] = x & 0xff;
    reg[4] = 0x00;
    reg[5] = (x1 >> 8) & 0xff;
    reg[6] = 0x00;
    reg[7] = x1 & 0xff;
    bsp_spi_send_recv(reg, NULL, 8);

    reg[0] = 0x00;
    reg[1] = 0x2B;
    bsp_gpio_write(_lcd_dc_pin, 0);
    bsp_spi_send_recv(reg, NULL, 2);
    bsp_gpio_write(_lcd_dc_pin, 1);

    reg[0] = 0x00;
    reg[1] = (y >> 8) & 0xff;
    reg[2] = 0x00;
    reg[3] = y & 0xff;
    reg[4] = 0x00;
    reg[5] = (y1 >> 8) & 0xff;
    reg[6] = 0x00;
    reg[7] = y1 & 0xff;
    bsp_spi_send_recv(reg, NULL, 8);

    reg[0] = 0x00;
    reg[1] = 0x2C;
    bsp_gpio_write(_lcd_dc_pin, 0);
    bsp_spi_send_recv(reg, NULL, 2);
    bsp_gpio_write(_lcd_dc_pin, 1);
}

static int lcd35a_ensure_flush_buf(uint32_t pixels) {
    if(_lcd35a_flush_pixels >= pixels)
        return 0;

    free(_lcd35a_flush_buf);
    _lcd35a_flush_buf = malloc(pixels * sizeof(uint16_t));
    if(_lcd35a_flush_buf == NULL) {
        _lcd35a_flush_pixels = 0;
        return -1;
    }
    _lcd35a_flush_pixels = pixels;
    return 0;
}

static int lcd35a_ensure_shadow(uint32_t pixels) {
    if(_lcd35a_shadow_pixels >= pixels)
        return 0;

    free(_lcd35a_shadow_argb);
    _lcd35a_shadow_argb = malloc(pixels * sizeof(uint32_t));
    if(_lcd35a_shadow_argb == NULL) {
        _lcd35a_shadow_pixels = 0;
        _lcd35a_shadow_ready = 0;
        return -1;
    }
    _lcd35a_shadow_pixels = pixels;
    _lcd35a_shadow_ready = 0;
    return 0;
}

static void lcd35a_pack_rect(const uint32_t* src, uint16_t x, uint16_t y,
        uint16_t w, uint16_t h) {
    uint32_t row;
    uint32_t col;
    uint32_t out = 0;

    for(row = 0; row < h; row++) {
        const uint32_t* line = src + ((uint32_t)(y + row) * LCD_WIDTH) + x;
        for(col = 0; col < w; col++)
            _lcd35a_flush_buf[out++] = lcd35a_argb_to_rgb565(line[col]);
    }
}

int  do_flush(const void* buf, uint32_t size) {
    uint32_t pixels;
    const uint32_t* src;
    uint32_t idx;
    uint32_t x;
    uint32_t y;
    uint32_t min_x;
    uint32_t max_x;
    uint32_t min_y;
    uint32_t max_y;
    uint32_t dirty_w;
    uint32_t dirty_h;
    uint32_t dirty_area;
    uint32_t total_area;
    uint8_t dirty;

    if(size != ((uint32_t)LCD_WIDTH * LCD_HEIGHT * 4))
        return 0;

    pixels = size >> 2;
    src = (const uint32_t*)buf;

    if(lcd35a_ensure_shadow(pixels) != 0)
        return -1;

    if(!_lcd35a_shadow_ready) {
        if(lcd35a_ensure_flush_buf(pixels) != 0)
            return -1;
        memcpy(_lcd35a_shadow_argb, src, pixels * sizeof(uint32_t));
        lcd35a_pack_rect(src, 0, 0, LCD_WIDTH, LCD_HEIGHT);

        lcd35a_start();
        lcd35a_set_window(0, 0, LCD_WIDTH, LCD_HEIGHT);
        bsp_spi_send_recv((const uint8_t*)_lcd35a_flush_buf, NULL, pixels * sizeof(uint16_t));
        lcd35a_end();
        _lcd35a_shadow_ready = 1;
        return 0;
    }

    min_x = LCD_WIDTH;
    max_x = 0;
    min_y = LCD_HEIGHT;
    max_y = 0;
    dirty = 0;

    for(y = 0; y < LCD_HEIGHT; y++) {
        for(x = 0; x < LCD_WIDTH; x++) {
            idx = y * LCD_WIDTH + x;
            if(_lcd35a_shadow_argb[idx] == src[idx])
                continue;
            _lcd35a_shadow_argb[idx] = src[idx];
            if(!dirty) {
                min_x = max_x = x;
                min_y = max_y = y;
                dirty = 1;
            }
            else {
                if(x < min_x) min_x = x;
                if(x > max_x) max_x = x;
                if(y < min_y) min_y = y;
                if(y > max_y) max_y = y;
            }
        }
    }

    if(!dirty)
        return 0;

    dirty_w = max_x - min_x + 1;
    dirty_h = max_y - min_y + 1;
    dirty_area = dirty_w * dirty_h;
    total_area = (uint32_t)LCD_WIDTH * LCD_HEIGHT;

    if(dirty_area > total_area / 3) {
        if(lcd35a_ensure_flush_buf(pixels) != 0)
            return -1;
        lcd35a_pack_rect(src, 0, 0, LCD_WIDTH, LCD_HEIGHT);

        lcd35a_start();
        lcd35a_set_window(0, 0, LCD_WIDTH, LCD_HEIGHT);
        bsp_spi_send_recv((const uint8_t*)_lcd35a_flush_buf, NULL, pixels * sizeof(uint16_t));
        lcd35a_end();
        return 0;
    }

    pixels = dirty_area;
    if(lcd35a_ensure_flush_buf(pixels) != 0)
        return -1;
    lcd35a_pack_rect(src, min_x, min_y, dirty_w, dirty_h);

    lcd35a_start();
    lcd35a_set_window(min_x, min_y, dirty_w, dirty_h);
    bsp_spi_send_recv((const uint8_t*)_lcd35a_flush_buf, NULL, pixels * sizeof(uint16_t));
    lcd35a_end();
    return 0;
}

static void lcd35a_apply_madctl(void) {
    lcd35a_start();
    lcd35a_write_command(0x36);
    lcd35a_write_data(lcd35a_madctl(_lcd_rot));
    lcd35a_end();
}

void lcd_init(uint32_t w, uint32_t h) {
    ILI9486_REG_WIDTH_16 = 1;
    ILI9486_INIT_PROFILE = ILI9486_INIT_PROFILE_GENERIC;
    ili9486_set_config(_lcd_dc_pin, _lcd_cs_pin, _lcd_rst_pin, _lcd_bl_pin,
            _lcd_spi_div, _lcd_spi_select);
    ili9486_init(w, h, _lcd_rot, _lcd_inversion,
            _lcd_dc_pin, _lcd_cs_pin, _lcd_rst_pin, _lcd_bl_pin, _lcd_spi_div);
    lcd35a_apply_madctl();
    _lcd35a_shadow_ready = 0;
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

static int doargs(int argc, char* argv[]) {
    int c = 0;
    while (c != -1) {
        c = getopt (argc, argv, "c:d:D:C:R:B:S:p:q:t:T:i:");
        if(c == -1)
            break;

        switch (c) {
        case 'c':
            _conf_file = optarg;
            break;
        case 'd':
            _lcd_spi_div = atoi(optarg);
            break;
        case 'D':
            _lcd_dc_pin = atoi(optarg);
            break;
        case 'C':
            _lcd_cs_pin = atoi(optarg);
            break;
        case 'R':
            _lcd_rst_pin = atoi(optarg);
            break;
        case 'B':
            _lcd_bl_pin = atoi(optarg);
            break;
        case 'S':
            _lcd_spi_select = atoi(optarg);
            break;
        case 'p':
            _tp_cs_pin = atoi(optarg);
            break;
        case 'q':
            _tp_irq_pin = atoi(optarg);
            break;
        case 't':
            _tp_spi_div = atoi(optarg);
            break;
        case 'T':
            _tp_spi_select = atoi(optarg);
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

int main(int argc, char** argv) {
    uint32_t w=480, h=320;
    LCD_HEIGHT = h;
    LCD_WIDTH = w;

    int opti = doargs(argc, argv);
    const char* mnt_point = (opti < argc && opti >= 0) ? argv[opti]: "/dev/disp0";
    lcd_init(w, h);
    xpt2046_set_config(_tp_cs_pin, _tp_irq_pin, _tp_spi_div, _tp_spi_select);
    xpt2046_init(_tp_cs_pin, _tp_irq_pin, _tp_spi_div);
    _tp_dev_name = mnt_point;

    displayd_t display;
    memset(&display, 0, sizeof(displayd_t));
    display.splash = NULL;
    display.flush = flush;
    display.init = init;
    display.get_info = get_info;
    display.read = tp_read;
    display.step_loop = tp_step;
    display.check_poll_events = tp_check_poll;
    int ret = fbdisplayd_run(&display, mnt_point, LCD_WIDTH, LCD_HEIGHT, _conf_file, _display_index);
    return ret;
}
