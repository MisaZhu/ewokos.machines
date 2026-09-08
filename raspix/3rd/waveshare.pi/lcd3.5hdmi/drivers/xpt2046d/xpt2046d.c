#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdbool.h>
#include <xpt2046/xpt2046.h>
#include <xpt2046/tp_filter.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/vfs.h>
#include <ewoksys/proc.h>

static int _spi_div = 256;
static int _tp_cs = 7;
static int _tp_irq = 25;

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

static int doargs(int argc, char* argv[]) {
    int c = 0;
    while (c != -1) {
        c = getopt(argc, argv, "c:i:d:");
        if(c == -1)
            break;

        switch (c) {
        case 'd':
            _spi_div = atoi(optarg);
            break;
        case 'c':
            _tp_cs = atoi(optarg);
            break;
        case 'i':
            _tp_irq = atoi(optarg);
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
 * then parked in vfs_block_by_fd() until tp_loop() asserts VFS_EVT_RD.
 */
static int tp_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        void* buf, int size, int offset, void* p) {
    (void)dev;
    (void)fd;
    (void)from_pid;
    (void)node;
    (void)offset;
    (void)p;

    if(!touch_has_data())
        return VFS_ERR_RETRY;
    if(size < 6)
        return -1;

    memcpy(buf, touch_data[touch_data_read % TOUCH_CACHE_SIZE], 6);
    touch_data_read++;
    return 6;
}

static uint32_t tp_check_poll_events(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node, void* p) {
    (void)dev;
    (void)fd;
    (void)from_pid;
    (void)node;
    (void)p;

    return touch_has_data() ? VFS_EVT_RD : 0;
}

/*
 * Sampler loop: polls the pen IRQ pin at TP_POLL_US cadence - a single
 * GPIO read per tick while idle, so an untouched panel costs nothing.
 * While pressed, each sample runs the clamp->median->average filter and
 * is queued: down/up edges are pushed immediately, moves only when the
 * filtered position actually changed (a stationary finger reports nothing).
 * The display is HDMI, so the touch has SPI0 to itself: no LCD CS dance.
 */
static int tp_loop(vdevice_t* dev, void* p) {
    (void)p;

    uint16_t press, x, y;
    int res = xpt2046_read(&press, &x, &y);

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
     * Level-triggered wakeup for /dev/touch0 readers: re-assert
     * VFS_EVT_RD while the queue still holds unread events, not only on
     * the push edge, so a blocked reader cannot sleep on data that is
     * already queued for it.
     */
    if(touch_has_data())
        vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);

    proc_usleep(TP_POLL_US);
    return 0;
}

int main(int argc, char** argv) {
    _spi_div = 256;
    _tp_cs = 7;
    _tp_irq = 25;

    int opti = doargs(argc, argv);
    const char* mnt_point = (opti < argc && opti >= 0) ? argv[opti]: "/dev/touch0";

    bsp_gpio_init();
    bsp_spi_init();

    xpt2046_init(_tp_cs, _tp_irq, _spi_div);

    vdevice_t dev;
    memset(&dev, 0, sizeof(vdevice_t));
    strcpy(dev.desc, "xpt2046");
    dev.loop_step = tp_loop;
    dev.read = tp_read;
    dev.check_poll_events = tp_check_poll_events;

    device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444, false);
    return 0;
}
