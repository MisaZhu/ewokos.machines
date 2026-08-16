#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <bsp/bsp_spi.h>
#include <displayd/displayd.h>
#include <ili9486/ili9486.h>
#include <xpt2046/xpt2046.h>

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

static uint32_t flush(const fbinfo_t* fbinfo, const graph_t* g) {
    uint32_t sz = 4 * g->w * g->h;
    do_flush(g->buffer, sz);
    return sz;
}

static fbinfo_t* get_info(void) {
    static fbinfo_t fbinfo;
    memset(&fbinfo, 0, sizeof(fbinfo_t));
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

int main(int argc, char** argv) {
    uint32_t w=480, h=320;
    LCD_HEIGHT = h;
    LCD_WIDTH = w;

    int opti = doargs(argc, argv);
    const char* mnt_point = (opti < argc && opti >= 0) ? argv[opti]: "/dev/disp0";
    lcd_init(w, h);
    xpt2046_set_config(_tp_cs_pin, _tp_irq_pin, _tp_spi_div, _tp_spi_select);
    xpt2046_init(_tp_cs_pin, _tp_irq_pin, _tp_spi_div);

    fbdisplayd_t display;
    memset(&display, 0, sizeof(fbdisplayd_t));
    display.splash = NULL;
    display.flush = flush;
    display.init = init;
    display.get_info = get_info;
    display.read = tp_read;
    int ret = fbdisplayd_run(&display, mnt_point, LCD_WIDTH, LCD_HEIGHT, _conf_file, _display_index);
    return ret;
}
