/******************************************************************************
 * Waveshare 3.7inch e-Paper HAT  -  displayd front-end (/dev/disp0)
 *
 *   epaper3.7d [-c conf.json] [-f sclk_hz] [-d spi_div] [-i index] [-t] [mount_point]
 *
 *   -f  Target SPI clock in Hz. The driver reads the real core clock from the
 *       firmware (250MHz on Pi1-3, 500MHz on Pi4 with force_turbo) and picks
 *       the power-of-two divider so SCLK <= this value. Default 2000000
 *       (2MHz, the official Waveshare bcm2835 demo speed).
 *   -d  Force a raw SPI0 divider instead (SCLK = core_clock / div). Only for
 *       experiments; 0 / omitted = derive from -f.
 *   -t  Draw the built-in self-test pattern for 15s before serving frames.
 ******************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <displayd/displayd.h>

#define LCD_WIDTH   280
#define LCD_HEIGHT  480
#define SPI_HZ_DEFAULT 2000000

int  do_flush(const void* rgb32, uint32_t w, uint32_t h);
void lcd_init(uint32_t w, uint32_t h, uint32_t rot, uint32_t spi_div, uint32_t spi_hz);
void lcd_selftest(void);

static const char* _conf_file = "";
static int _display_index = 0;
static int _spi_div = 0;                 /* 0 = derive from _spi_hz */
static int _spi_hz = SPI_HZ_DEFAULT;
static int _selftest = 0;

static uint32_t flush(const disp_info_t* fbinfo, const graph_t* g) {
    (void)fbinfo;
    if(do_flush(g->buffer, g->w, g->h) != 0)
        return 0;
    return 4 * g->w * g->h;
}

static disp_info_t* get_info(void) {
    static disp_info_t fbinfo;
    memset(&fbinfo, 0, sizeof(disp_info_t));
    fbinfo.width  = LCD_WIDTH;
    fbinfo.height = LCD_HEIGHT;
    fbinfo.depth  = 32;
    return &fbinfo;
}

static int32_t init(uint32_t w, uint32_t h, uint32_t dep) {
    (void)w; (void)h; (void)dep;
    return 0;
}

static int doargs(int argc, char* argv[]) {
    int c;
    while((c = getopt(argc, argv, "c:d:f:i:t")) != -1) {
        switch(c) {
        case 'c':
            _conf_file = optarg;
            break;
        case 'd':
            _spi_div = atoi(optarg);
            if(_spi_div < 2)
                _spi_div = 0;            /* back to auto */
            break;
        case 'f':
            _spi_hz = atoi(optarg);
            if(_spi_hz <= 0)
                _spi_hz = SPI_HZ_DEFAULT;
            break;
        case 'i':
            _display_index = atoi(optarg);
            break;
        case 't':
            _selftest = 1;
            break;
        default:
            break;
        }
    }
    return optind;
}

int main(int argc, char** argv) {
    int opti = doargs(argc, argv);
    const char* mnt_point = (opti < argc) ? argv[opti] : "/dev/disp0";

    lcd_init(LCD_WIDTH, LCD_HEIGHT, G_ROTATE_0, (uint32_t)_spi_div, (uint32_t)_spi_hz);
    if(_selftest)
        lcd_selftest();

    displayd_t display;
    memset(&display, 0, sizeof(displayd_t));
    display.splash   = NULL;
    display.flush    = flush;
    display.init     = init;
    display.get_info = get_info;
    return fbdisplayd_run(&display, mnt_point, LCD_WIDTH, LCD_HEIGHT, _conf_file, _display_index);
}
