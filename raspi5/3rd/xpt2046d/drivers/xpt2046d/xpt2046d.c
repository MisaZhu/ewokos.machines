#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdbool.h>
#include <displayd/displayd.h>
#include <xpt2046/xpt2046.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/vfs.h>
#include <ewoksys/proc.h>

static int _spi_div = 128;
static int _tp_cs = 7;
static int _tp_irq = 25;

/* Touch position filtering pipeline (classic combo):
 *   raw ADC -> clamp (reject jumps) -> median (kill impulses)
 *           -> moving average (smooth) -> output
 * Filters are re-seeded on every pen-down edge so a new touch never
 * inherits the previous touch's coordinates; on release the last
 * filtered position is reported. */

#define TP_MAX_JUMP  400 /* max plausible delta between two samples (~1/10 of the 12-bit range) */
#define TP_MEDIAN_N  5   /* median window (odd) */
#define TP_AVG_N     4   /* moving average window */

typedef struct {
    uint16_t med[TP_MEDIAN_N];
    uint8_t  med_n;   /* samples filled in the median window */
    uint8_t  med_idx; /* median ring write position */
    uint16_t avg[TP_AVG_N];
    uint8_t  avg_n;
    uint8_t  avg_idx;
    uint16_t last;    /* last accepted (clamped) sample */
    uint16_t out;     /* last filtered output */
    uint8_t  seeded;  /* clamp reference valid */
} tp_filter_t;

static tp_filter_t _fx, _fy;
static bool _pen_down = false;

static void tp_filter_reset(tp_filter_t* f) {
    f->med_n = f->med_idx = 0;
    f->avg_n = f->avg_idx = 0;
    f->seeded = 0;
    /* keep f->out: release events still report the last filtered position */
}

static uint16_t tp_filter_in(tp_filter_t* f, uint16_t v) {
    uint8_t i;

    /* 1. clamp: reject samples jumping too far from the last accepted one */
    if(f->seeded) {
        int32_t d = (int32_t)v - (int32_t)f->last;
        if(d > TP_MAX_JUMP || d < -TP_MAX_JUMP)
            v = f->last;
    }
    f->last = v;

    /* 2. median over the clamped window (kills impulse noise) */
    f->med[f->med_idx] = v;
    f->med_idx = (f->med_idx + 1) % TP_MEDIAN_N;
    if(f->med_n < TP_MEDIAN_N)
        f->med_n++;

    uint16_t tmp[TP_MEDIAN_N];
    memcpy(tmp, f->med, f->med_n * sizeof(uint16_t));
    for(i = 1; i < f->med_n; i++) { /* insertion sort, N is tiny */
        uint16_t k = tmp[i];
        int8_t j = (int8_t)i - 1;
        while(j >= 0 && tmp[j] > k) {
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = k;
    }
    uint16_t m = tmp[f->med_n / 2];

    /* 3. moving average over the median outputs (smoothing) */
    f->avg[f->avg_idx] = m;
    f->avg_idx = (f->avg_idx + 1) % TP_AVG_N;
    if(f->avg_n < TP_AVG_N)
        f->avg_n++;

    uint32_t s = 0;
    for(i = 0; i < f->avg_n; i++)
        s += f->avg[i];
    f->out = (uint16_t)(s / f->avg_n);
    f->seeded = 1;
    return f->out;
}

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
        c = getopt (argc, argv, "c:i:d:");
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
 * While pressed, each sample runs the filter pipeline and is queued:
 * down/up edges are pushed immediately, moves only when the filtered
 * position actually changed (a stationary finger reports nothing).
 */
static int tp_loop(vdevice_t* dev, void* p) {
    (void)p;

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
     * Level-triggered wakeup for /dev/touch0 readers: re-assert
     * VFS_EVT_RD while the queue still holds events, not only on
     * the push edge, so a blocked reader cannot sleep on data that is
     * already queued for it.
     */
    if(touch_has_data())
        vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);

    proc_usleep(TP_POLL_US);
    return 0;
}

int main(int argc, char** argv) {
    _spi_div = 128;
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
