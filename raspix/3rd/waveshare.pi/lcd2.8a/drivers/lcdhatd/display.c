#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdbool.h>
#include <displayd/displayd.h>
#include <ili9486/ili9486.h>
#include <xpt2046/xpt2046.h>
#include <xpt2046/tp_filter.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/vfs.h>
#include <ewoksys/proc.h>

static tp_filter_t _fx, _fy;
static bool _pen_down = false;

/*
 * Event queue between the sampler loop and readers: fixed 6-byte events
 * (state, x, y), same wire format as before. A wedged reader drops the
 * oldest event; a blocked reader wakes on the first queued one.
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

/* mount point of this display device, for the vfs_wakeup() node lookup */
static const char* _tp_dev_name = "/dev/disp0";

int  do_flush(const void* buf, uint32_t size) {
    ili9486_flush(buf, size);
    return 0;
}

void lcd_init(uint32_t w, uint32_t h, uint32_t div) {
    const int lcd_dc = 22;
    const int lcd_cs = 8;
    const int lcd_rst = 27;
    const int lcd_bl = 18;
    ili9486_init(w, h, G_ROTATE_90, 0, lcd_dc, lcd_cs, lcd_rst, lcd_bl, div);
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

static int _spi_div = 8;
static int _display_index = 0;
const char* _conf_file = "";
static int doargs(int argc, char* argv[]) {
    int c = 0;
    while (c != -1) {
        c = getopt(argc, argv, "c:d:i:");
        if(c == -1)
            break;

        switch (c) {
        case 'd':
            _spi_div = atoi(optarg);
            break;
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

/*
 * Blocking read: pops one queued event; with an empty queue returns
 * VFS_ERR_RETRY, which libc turns into EAGAIN - a blocking reader is
 * then parked in vfs_block_by_fd() until tp_step() asserts VFS_EVT_RD.
 */
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
 * Sampler loop (displayd_t.step_loop, same pipeline as the standalone
 * xpt2046d): polls the pen IRQ pin at TP_POLL_US cadence - a single GPIO
 * read per tick while idle, so an untouched panel costs nothing. While
 * pressed, each sample runs the clamp->median->average filter and is
 * queued: down/up edges are pushed immediately, moves only when the
 * filtered position actually changed (a stationary finger reports nothing).
 *
 * The LCD and the XPT2046 share SPI0: a flush IPC preempts this loop
 * between samples, and the LCD CS is parked deselected for the duration of
 * a touch poll so stray touch bytes never reach the panel's GRAM. Any
 * sample clobbered by the preemption is rejected by the clamp filter.
 */
static int tp_step(void) {
    /* fbdisplayd_run() creates the display mount itself, so resolve the
       node lazily for the vfs_wakeup() calls */
    static ewokos_addr_t node = 0;

    uint16_t press, x, y;
    bsp_gpio_write(8, 1);
    int res = xpt2046_read(&press, &x, &y);
    bsp_gpio_write(8, 0);

    if(res == 0) {
        if(press != 0) {
            if(!_pen_down) { /* pen-down edge: reseed the filters */
                tp_filter_reset(&_fx);
                tp_filter_reset(&_fy);
                _pen_down = true;
                touch_push(1, tp_filter_in(&_fx, x), tp_filter_in(&_fy, y));
            }
            else {
                uint16_t px = _fx.out;
                uint16_t py = _fy.out;
                uint16_t fx = tp_filter_in(&_fx, x);
                uint16_t fy = tp_filter_in(&_fy, y);
                if(fx != px || fy != py)
                    touch_push(1, fx, fy);
            }
        }
        else { /* release edge: report the last filtered position */
            _pen_down = false;
            tp_filter_reset(&_fx);
            tp_filter_reset(&_fy);
            touch_push(0, _fx.out, _fy.out);
        }
    }

    /*
     * Level-triggered wakeup for display-node readers: re-assert
     * VFS_EVT_RD while the queue still holds unread events, not only on
     * the push edge, so a blocked reader cannot sleep on data that is
     * already queued for it.
     */
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
    _spi_div = 16;
    uint32_t w=320, h=240;
    LCD_HEIGHT = h;
    LCD_WIDTH = w;

    int opti = doargs(argc, argv);
    const char* mnt_point = (opti < argc && opti >= 0) ? argv[opti]: "/dev/disp0";

    lcd_init(w, h, _spi_div);

    const int tp_cs = 7;
    const int tp_irq = 17;
    xpt2046_init(tp_cs, tp_irq, 64);

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
