/*
 * himaxd - Huawei MateBook gaokun3 HX83121A touchscreen daemon.
 *
 * The TW220 Goodix path (drivers/q8b/goodixd) is a different IC and stays
 * untouched; this daemon implements the Himax HX83121A that the gaokun3 panel
 * carries.  Both serve the same six-byte /dev/touch0 record contract, so the
 * existing xtouch client works unchanged.
 *
 * Hardware (gaokun3 board configuration, cross-checked against the Linux
 * device tree and driver used on working gaokun3 systems):
 *
 *   MISO gpio154, MOSI gpio155, CLK gpio156, CS gpio157  (QUP0 SE6)
 *   IRQ gpio175 level-low, RESET gpio99 active-low
 *   host-interface select gpio174: must be low before the touch firmware
 *   reloads, otherwise the IC reports over I2C-HID and SPI stays silent
 *   panel power rails are always on for this board (no enable GPIO)
 *
 * In SPI mode the IC does not send processed coordinates; it keeps a raw
 * 60x40 capacitance frame available and the host reads the latest one.
 * hx_algo.c (ported from the community Linux driver) filters the frame and
 * solves contacts.
 *
 * Transport is a GPIO bit-bang master; see himax_gpio.c for why the GENI
 * controller is not used.  One frame is ~5 KiB, so frame handling is
 * rate-limited and the daemon does one blocking read per interrupt.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ewoksys/kernel_tic.h>
#include <ewoksys/klog.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/vfs.h>

#include "himax_gpio.h"
#include "himax_protocol.h"
#include "hx_algo.h"

/* Raw output space of the solver: grid columns map to 2560 px (60 cells),
 * grid rows to 1600 px (40 cells).  xtouch applies the final orientation
 * through /etc/x/xtouch-himax.json. */
#define TOUCH_X_MAX               2560U
#define TOUCH_Y_MAX               1600U

#define TOUCH_QUEUE_SIZE          16U
#define PROBE_RETRY_INITIAL_US    250000U
#define PROBE_RETRY_MAX_US        30000000U
/*
 * Sampling budget.  One frame costs ~16 ms of bit-bang on this panel, so the
 * CPU load is directly frame rate * read time.  80 ms keeps ~12 fps and
 * roughly a quarter of one core; change it with the optional frame-ms
 * argument or the HIMAX_FRAME_MS env var.
 */
#define FRAME_BUDGET_MS           80U
#define FRAME_BUDGET_MIN_MS       20U
#define FRAME_BUDGET_MAX_MS       500U
/* Best effort sync window: the panel pulses the level-low line ~25 us every
 * ~8 ms; a short spin at the start of each budget catches it so the read
 * starts on a record boundary instead of a torn slice. */
#define SYNC_SPIN_US              3000U
#define FRAME_RETRY_MAX           2U

typedef struct {
    uint16_t state;
    uint16_t x;
    uint16_t y;
} touch_record_t;

static himax_dev_t* _himax;
static struct hx_algo _algo;
static uint8_t _stream[HIMAX_STREAM_BYTES];
static struct input_mt_pos _pos[HIMAX_MAX_TOUCH];

static bool _ready;
static touch_record_t _queue[TOUCH_QUEUE_SIZE];
static unsigned _queue_count;
static bool _finger_down;
static uint16_t _finger_x;
static uint16_t _finger_y;
static unsigned _probe_delay_us = PROBE_RETRY_INITIAL_US;
static unsigned _probe_failures;
static uint64_t _last_frame_ms;
static unsigned _read_failures;
static const char* _last_err_stage;
static unsigned _bad_frames;
static unsigned _frames;
static unsigned _invalid_frames;
static unsigned _contacts_seen;
static unsigned _presses;
static unsigned _releases;
static unsigned _skipped_pulse;
static unsigned _frame_budget_ms = FRAME_BUDGET_MS;
static uint64_t _loop_busy_us;
static uint64_t _loop_start_ms;
static uint64_t _busy_from_ms;
static uint64_t _iter_end_ms;

static void queue_push(uint16_t state, uint16_t x, uint16_t y) {
    if (_queue_count == TOUCH_QUEUE_SIZE)
        return;
    _queue[_queue_count].state = state;
    _queue[_queue_count].x = x;
    _queue[_queue_count].y = y;
    _queue_count++;
}

static unsigned clamp_u16(int value, unsigned limit) {
    if (value < 0)
        return 0;
    if ((unsigned)value > limit)
        return limit;
    return (unsigned)value;
}

/*
 * Feed one raw frame through the ported pipeline and publish the primary
 * contact.  The /dev/touch0 contract carries a single contact, matching the
 * TW220 driver; further fingers are solved and tracked but not reported.
 */
static void process_frame(vdevice_t* dev) {
    const uint16_t* raw = (const uint16_t*)(_stream + HIMAX_STREAM_HEADER_BYTES);
    int count;
    int stable;
    bool report_on = true;

    hx_preprocess_frame(&_algo, raw);
    hx_detect_macro_zones(&_algo);
    hx_reject_palms(&_algo);
    hx_detect_peaks(&_algo);
    hx_expand_and_resolve(&_algo, _pos, &count);
    hx_track_contacts(&_algo, _pos, count);
    stable = hx_count_stable_tracks(&_algo);

    if (stable > 0 && !_algo.touch_active) {
        _algo.touch_start_frames++;
        if (_algo.touch_start_frames < _algo.track_start_debounce)
            report_on = false;
        else
            _algo.touch_active = true;
    } else if (stable == 0) {
        _algo.touch_start_frames = 0;
        _algo.touch_active = false;
    }

    if (report_on) {
        int first = -1;
        for (int i = 0; i < HIMAX_MAX_TOUCH; i++) {
            if (_algo.tracks[i].active && _algo.tracks[i].debounce == 0) {
                first = i;
                break;
            }
        }
        if (first >= 0) {
            _finger_x = (uint16_t)clamp_u16(_algo.tracks[first].x,
                    TOUCH_X_MAX - 1U);
            _finger_y = (uint16_t)clamp_u16(_algo.tracks[first].y,
                    TOUCH_Y_MAX - 1U);
            queue_push(1, _finger_x, _finger_y);
            _contacts_seen++;
            if (!_finger_down)
                _presses++;
            _finger_down = true;
        } else if (_finger_down) {
            /* Keep the last contact position: xtouch/xserverd only turn a
             * release into a click near the original press point. */
            queue_push(0, _finger_x, _finger_y);
            _finger_down = false;
            _releases++;
        }
    }
    if (_queue_count != 0)
        vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
}

/* Small status file, rewritten and closed so another process can read it:
 * EwokOS fflush() is a no-op and VFS publishes on close. */
static void write_status(void) {
    char line[256];
    FILE* f = fopen("/tmp/himaxd-status", "w");

    if (!f)
        return;
    snprintf(line, sizeof(line),
            "uptime_ms %llu frames %u invalid %u ready %u queue %u "
            "contacts %u presses %u releases %u skipped_pulse %u "
            "fps %u busy_pct %u\n",
            (unsigned long long)kernel_tic_ms(0), _frames, _invalid_frames,
            _ready ? 1U : 0U, _queue_count, _contacts_seen, _presses,
            _releases, _skipped_pulse,
            _loop_start_ms ? (unsigned)(_frames * 1000ULL /
                    (kernel_tic_ms(0) - _loop_start_ms)) : 0U,
            _loop_busy_us ? (unsigned)(_loop_busy_us * 100ULL /
                    (kernel_tic_ms(0) - _loop_start_ms) / 1000ULL) : 0U);
    fputs(line, f);
    fclose(f);
}

/* Gap-based sync: skip the pulse wait.  The budget sleep is the settle gap;
 * reads that follow it are clean (validated on device with a single daemon).
 * Saves the ~5 ms/frame the spin version burned. */
static bool sync_irq(void) {
    return true;
}

static bool probe_ic(void) {
    if (himax_probe(_himax) == 0) {
        slog("gaokun3 himaxd: HX%06x ready, raw frame %ux%u\n",
                _himax->ic_id, HIMAX_COLS, HIMAX_ROWS);
        return true;
    }
    /* Log the transition and then only powers of two: a dead panel must not
     * write one line per retry forever. */
    if (_probe_failures == 0 || _last_err_stage != _himax->stage ||
            (_probe_failures & (_probe_failures - 1U)) == 0)
        slog("gaokun3 himaxd: probe failed at %s (bus errors %u, retry %u us)\n",
                _himax->stage, _himax->bus_errors, _probe_delay_us);
    _last_err_stage = _himax->stage;
    return false;
}

static void schedule_retry(void) {
    unsigned delay = _probe_delay_us;
    unsigned next;

    _probe_failures++;
    if (delay == 0)
        next = PROBE_RETRY_INITIAL_US;
    else if (delay >= PROBE_RETRY_MAX_US / 2U)
        next = PROBE_RETRY_MAX_US;
    else
        next = delay * 2U;
    _probe_delay_us = next;
}

static void recover_ic(void) {
    _ready = false;
    _algo.iir_initialized = false;
    _algo.touch_active = false;
    _algo.touch_start_frames = 0;
    memset(_algo.tracks, 0, sizeof(_algo.tracks));
    _read_failures++;
    if (_read_failures <= 3 || (_read_failures & (_read_failures - 1U)) == 0)
        slog("gaokun3 himaxd: frame read failed at %s, re-initializing (%u)\n",
                _himax->stage, _read_failures);
}

static int touch_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        void* buf, int size, off_t offset, void* p) {
    touch_record_t* out;
    (void)dev; (void)fd; (void)from_pid; (void)node; (void)offset; (void)p;
    if (size < (int)sizeof(touch_record_t) || _queue_count == 0)
        return VFS_ERR_RETRY;
    out = (touch_record_t*)buf;
    *out = _queue[0];
    memmove(&_queue[0], &_queue[1], (_queue_count - 1) * sizeof(_queue[0]));
    _queue_count--;
    return sizeof(*out);
}

static uint32_t touch_poll(vdevice_t* dev, int fd, int from_pid,
        fsinfo_t* node, void* p) {
    (void)dev; (void)fd; (void)from_pid; (void)node; (void)p;
    return _queue_count != 0 ? VFS_EVT_RD : 0;
}

static int touch_loop(vdevice_t* dev, void* p) {
    (void)p;
    if (!_ready) {
        unsigned delay = _probe_delay_us;
        if (delay != 0)
            usleep(delay);
        if (probe_ic()) {
            _ready = true;
            _probe_failures = 0;
            _probe_delay_us = 0;
            _read_failures = 0;
            _last_frame_ms = 0;
        } else {
            schedule_retry();
        }
        return 0;
    }
    /*
     * Fixed-budget sampler: sleep most of the budget, spin briefly for the
     * record pulse, read one record, validate, and only then run the solver.
     * The budget keeps CPU bounded (load = budget / read cost); the short
     * sync keeps the read on a record boundary (a torn slice is rejected and
     * retried).  A missed pulse still reads: the validator decides.
     */
    {
        uint64_t now = kernel_tic_ms(0);
        if (_last_frame_ms != 0) {
            uint64_t elapsed = now - _last_frame_ms;
            if (elapsed < _frame_budget_ms)
                usleep((_frame_budget_ms - (unsigned)elapsed) * 1000U);
        }
        _busy_from_ms = kernel_tic_ms(0);
    }
    if (!sync_irq())
        _skipped_pulse++;
    for (unsigned attempt = 0; attempt <= FRAME_RETRY_MAX; attempt++) {
        if (himax_read_frame(_himax, _stream) != 0) {
            recover_ic();
            return 0;
        }
        _frames++;
        if (himax_frame_valid(_stream)) {
            _bad_frames = 0;
            process_frame(dev);
            if ((_frames & 31U) == 0)
                write_status();
            break;
        }
        _invalid_frames++;
        _bad_frames++;
        if (attempt < FRAME_RETRY_MAX)
            (void)sync_irq();
    }
    {
        /* Coarse busy estimate: ms spent in this iteration excluding the
         * budget sleep.  (kernel_tic_ms is tick-quantized, so this is an
         * approximation; fine for a status line.) */
        uint64_t now = kernel_tic_ms(0);
        if (_iter_end_ms) {
            uint64_t spent = now - _busy_from_ms;
            _loop_busy_us += spent * 1000ULL;
        }
        _busy_from_ms = 0;
        _iter_end_ms = now;
    }
    _last_frame_ms = kernel_tic_ms(0);
    if (_loop_start_ms == 0)
        _loop_start_ms = _last_frame_ms;
    return 0;
}

int main(int argc, char** argv) {
    const char* mount = argc > 1 ? argv[1] : "/dev/touch0";
    vdevice_t dev;
    const char* budget_env;

    if (argc > 3 || mount[0] != '/') {
        fprintf(stderr, "usage: himaxd [/dev/touch0] [frame-ms]\n");
        return -1;
    }
    if (argc == 3) {
        long v = strtol(argv[2], NULL, 10);
        if (v >= FRAME_BUDGET_MIN_MS && v <= FRAME_BUDGET_MAX_MS)
            _frame_budget_ms = (unsigned)v;
    }
    budget_env = getenv("HIMAX_FRAME_MS");
    if (budget_env) {
        long v = strtol(budget_env, NULL, 10);
        if (v >= FRAME_BUDGET_MIN_MS && v <= FRAME_BUDGET_MAX_MS)
            _frame_budget_ms = (unsigned)v;
    }
    slog("gaokun3 himaxd: frame budget %u ms\n", _frame_budget_ms);
    if (!himax_gpio_open())
        return -1;
    _himax = himax_gpio_dev();
    memset(&_algo, 0, sizeof(_algo));
    hx_algo_init_defaults(&_algo);
    /*
     * A tap on this bus spans only a few frames (one frame is ~40 ms here),
     * so the default new-contact debounce drops short taps entirely.  Report
     * the first stable frame and release after two missed frames.
     */
    _algo.debounce_base = 0;
    _algo.track_start_debounce = 1;
    _algo.track_lost_frames = 2;
    /* Short taps are weaker than a parked finger; keep detection above the
     * measured idle noise (~±120) but below a light tap. */
    _algo.macro_threshold = 600;
    _algo.peak_threshold = 600;

    memset(&dev, 0, sizeof(dev));
    strcpy(dev.desc, "gaokun3_himax_hx83121a");
    dev.read = touch_read;
    dev.check_poll_events = touch_poll;
    dev.loop_step = touch_loop;
    return device_run(&dev, mount, FS_TYPE_CHAR, 0444, false);
}
