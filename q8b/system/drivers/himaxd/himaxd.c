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

extern himax_dev_t* himax_native_transport_open(void);

/* Raw output space of the solver: grid columns map to 2560 px (60 cells),
 * grid rows to 1600 px (40 cells).  xtouch applies the final orientation
 * through /etc/x/xtouch-himax.json. */
#define TOUCH_X_MAX               2560U
#define TOUCH_Y_MAX               1600U

#define TOUCH_QUEUE_SIZE          16U
/* First retry is quick; the cap below bounds the worst case. */
#define PROBE_RETRY_INITIAL_US    50000U
/*
 * Upper bound on the probe retry backoff.
 *
 * A panel that stops answering is a transient (a bus glitch, or the IC still
 * finishing a reset), so retrying must not cost a perceptible freeze.  This
 * cap used to be 30 s: one failed probe parked the sampler for half a minute,
 * and every touch in that window was lost.  Out-of-tree measurements put the
 * perceived delay at 0.6-2.1 s with whole frames missing, which is exactly
 * this backoff, not a slow read.  Keep it well below one second.
 */
#define PROBE_RETRY_MAX_US        1000000U
/*
 * A full probe is a whole bring-up sequence (raw-report selector, firmware
 * status, chip detect, init) - tens of SPI transactions, far more bus time
 * than the 5.4 ms a record costs.  Hammering it every 250 ms against an IC
 * that is not answering keeps the bus busy permanently and starves the
 * records that DO get through.  Retry quickly at first (a glitch clears at
 * once), then back off to the cap above so a dead panel is not polled hard.
 */
#define PROBE_RETRY_FAST_TRIES    4U
/*
 * Sampling budget.  One frame costs ~16 ms of bit-bang on this panel, so the
 * CPU load is directly frame rate * read time.  80 ms keeps ~12 fps and
 * roughly a quarter of one core; change it with the optional frame-ms
 * argument or the HIMAX_FRAME_MS env var.
 */
/*
 * Sampling budget.  At 9.6 MHz one GENI record read is ~5.4 ms, so the panel
 * frame rate (~20 fps), not the transfer, is the limit.  Measured on the
 * gaokun3 panel:
 *
 *   20 ms -> 24 fps, ~30% of one core
 *   30 ms -> 20 fps, ~23%          <- default: still the panel's rate
 *   50 ms -> 14 fps, ~14%
 *
 * Rejected records are handled by the marker check
 * (himax_read_frame_synced), never by slowing the budget down.
 */
#define FRAME_BUDGET_MS           30U
#define FRAME_BUDGET_MIN_MS       20U
#define FRAME_BUDGET_MAX_MS       500U
/* Best effort sync window: the panel pulses the level-low line ~25 us every
 * ~8 ms; a short spin at the start of each budget catches it so the read
 * starts on a record boundary instead of a torn slice. */
#define SYNC_SPIN_US              3000U
#define FRAME_RETRY_MAX           2U
/* Settle time before re-reading a rejected record; see touch_loop(). */
#define FRAME_RETRY_GAP_US        8000U
/*
 * Torn reads are retried inline, with no gap and no reset: the next record's
 * header is the resync point (see the note in the read loop).  This is the
 * budget for those immediate retries - the retry that finally catches a
 * record boundary costs no more than one record read each time.
 */
#define FRAME_RETRY_MAX_EXTRA     6U
/*
 * Consecutive reads that never found a record header before the IC is
 * considered dead rather than merely mid-update.  At ~5.4 ms per read this is
 * well under the budget the old "reset on the first torn read" path cost, and
 * it only fires when the panel has genuinely stopped producing records.
 */
#define FRAME_READ_FAIL_RECOVER   32U
/*
 * Sampling iterations that may pass with no usable record at all before the
 * IC is re-probed.  Each iteration already retries several reads, so this is
 * a long silence (seconds), not a busy panel.
 */
#define DEAD_ROUNDS_RECOVER       20U
static unsigned _silent_rounds;
static unsigned _torn_reads;

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
static unsigned _recoveries;
static const char* _last_err_stage;
static unsigned _bad_frames;
static unsigned _frames;
static unsigned _invalid_frames;
static uint16_t _last_seq;
static unsigned _duplicate_frames;
static unsigned _contacts_seen;
static unsigned _presses;
static unsigned _releases;
static unsigned _skipped_pulse;
static unsigned _frame_budget_ms = FRAME_BUDGET_MS;
static uint64_t _loop_busy_us;
static uint64_t _loop_start_ms;
static uint64_t _busy_from_ms;
static uint64_t _iter_end_ms;

static bool _trace_events;
static FILE* _trace_fp;
static FILE* _probe_fp;
static FILE* _heart_fp;
static unsigned _heart_ticks;

static void trace_f(FILE* fp, const char* fmt, ...) {
    if (!fp) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fflush(fp);
}

static void heart(void) {
    if (_heart_fp) {
        _heart_ticks++;
        if ((_heart_ticks & 1) == 1) {
            fprintf(_heart_fp, "tick %u\n", _heart_ticks);
            fflush(_heart_fp);
        }
    }
}

static unsigned _queue_drops;

/*
 * Queue a position, discarding the OLDEST one when full.
 *
 * Dropping the newest (the previous behaviour) is what made a slow client see
 * a freeze followed by a catch-up: the queue filled with the last 16 positions
 * the finger had already left, the client then had to replay all of them in
 * order before it could draw where the finger actually was now, so the cursor
 * lagged by however far behind the queue had drifted.  A touch stream is only
 * useful at its leading edge: when we cannot keep every sample, the newest
 * position is the one worth having.
 */
static void queue_push(uint16_t state, uint16_t x, uint16_t y) {
    if (_queue_count == TOUCH_QUEUE_SIZE) {
        /* Full: forget the oldest sample and keep the newest. */
        memmove(&_queue[0], &_queue[1],
                (_queue_count - 1) * sizeof(_queue[0]));
        _queue_count--;
        _queue_drops++;
    }
    _queue[_queue_count].state = state;
    _queue[_queue_count].x = x;
    _queue[_queue_count].y = y;
    _queue_count++;
    if (_trace_events) {
        trace_f(_trace_fp, "touch state=%u x=%u y=%u\n", state, x, y);
        slog("gaokun3 himaxd: touch state=%u x=%u y=%u\n", state, x, y);
    }
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
/*
 * The raw-report selector can be lost while the daemon is sampling.
 *
 * FW_STATUS still says RUNNING and the event stack keeps producing
 * well-formed records, so nothing looks wrong except the one thing that
 * matters: the grid stops changing, the solver stops finding contacts, and
 * the touch stream simply ends.  A firmware reload or a re-init resets it to
 * 0, so re-assert it whenever it is observed to have drifted - cheap (one
 * register read every few seconds) next to a stream that silently dies.
 */
static uint64_t _raw_sel_check_ms;

static void keep_raw_out_sel(void) {
    uint64_t now = kernel_tic_ms(0);
    uint8_t cur[4] = { 0, 0, 0, 0 };
    static const uint8_t want[4] = { HIMAX_RAW_OUT_SELECT, 0, 0, 0 };

    if (_raw_sel_check_ms != 0 && now - _raw_sel_check_ms < 3000U)
        return;
    _raw_sel_check_ms = now;
    if (himax_reg_read(_himax, HIMAX_DSRAM_RAW_OUT_SEL, cur, 4) != 0)
        return;
    if (memcmp(cur, want, 4) == 0)
        return;
    if (himax_reg_write(_himax, HIMAX_DSRAM_RAW_OUT_SEL, want, 4) == 0)
        slog("gaokun3 himaxd: raw_out_sel was %02x, re-asserted %02x\n",
                cur[0], HIMAX_RAW_OUT_SELECT);
}

/* Raw-signal envelope of the last frame, published by "status" so the input
 * the solver actually sees can be compared against the thresholds it applies. */
static uint16_t _raw_min, _raw_max, _raw_first;
static unsigned _raw_above_threshold;

static void process_frame(vdevice_t* dev) {
    const uint16_t* raw = (const uint16_t*)(_stream + HIMAX_STREAM_HEADER_BYTES);
    int count;
    int stable;
    bool report_on = true;

    {
        unsigned lo = 0xffffU, hi = 0, above = 0;
        for (unsigned i = 0; i < HIMAX_PIXELS; i++) {
            unsigned v = raw[i];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
            if (v >= _algo.macro_threshold) above++;
        }
        _raw_min = (uint16_t)lo;
        _raw_max = (uint16_t)hi;
        _raw_first = raw[0];
        _raw_above_threshold = above;
    }

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
 * EwokOS fflush() is a no-op and VFS publishes on close.  Only ever called
 * from the IPC thread - never from the sampler. */
static void write_status(void) {
    char line[256];
    FILE* f = fopen("/tmp/himaxd-status", "w");

    if (!f)
        return;
    snprintf(line, sizeof(line),
            "uptime_ms %llu frames %u invalid %u ready %u queue %u "
            "contacts %u presses %u releases %u skipped_pulse %u "
            "duplicates %u torn %u fps %u busy_pct %u\n",
            (unsigned long long)kernel_tic_ms(0), _frames, _invalid_frames,
            _ready ? 1U : 0U, _queue_count, _contacts_seen, _presses,
            _releases, _skipped_pulse, _duplicate_frames, _torn_reads,
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

static bool _native_probe;
/* Set by recover_ic(): the next probe must run the whole bring-up instead of
 * trusting an IC that merely answers.  A firmware that reports RUNNING with a
 * zeroed raw-report selector is exactly the state that must not be trusted,
 * and it is indistinguishable from a healthy one by frame reads alone. */
static bool _force_full_bringup;

/* How many live-record reads to try before running the IC bring-up.  Each
 * read is ~43 ms, so this costs at most ~0.3 s once per process start. */
#define LIVE_FRAME_TRIES 6U

static bool probe_ic(void) {
    int ok;
    if (_native_probe) {
        /*
         * The GENI transport can already reach the IC that the boot firmware
         * left running: a record comes back complete and valid.  The full
         * bring-up is only needed when that is not the case, and running it
         * against a live IC actually drops the firmware into SAFE_MODE
         * (FW_STATUS 0x0c), after which no frame is valid any more.  So try a
         * plain frame first and leave the IC alone when it already works.
         */
        /*
         * The raw-report selector is the difference between an IC that reads
         * back as RUNNING and one that actually produces records: after a
         * cold boot 0x100072ec reads 0 and the event stack stays all zeros,
         * while a working panel has it at 0xf6.  Set it before probing, so a
         * freshly booted board streams without needing a full re-init.
         */
        /*
         * A panel left in SAFE_MODE is the one failure that looks healthy
         * from every angle except the one that matters: the IC answers, the
         * event stack keeps producing well-formed records, and the frame
         * counters climb - but the grid never changes, so no touch is ever
         * detected.  Leaving it there is what made the driver report
         * "ready, 25 fps, invalid 0" while the screen ignored every finger.
         * Break out of it before anything else.
         */
        {
            uint8_t st[4] = { 0, 0, 0, 0 };
            if (himax_reg_read(_himax, HIMAX_REG_FW_STATUS, st, 4) == 0 &&
                    st[0] == HIMAX_FW_STATE_SAFE_MODE) {
                trace_f(_probe_fp, "probe native: in SAFE_MODE, leaving\n");
                (void)himax_leave_safe_mode(_himax);
            }
        }
        {
            static const uint8_t raw_out[4] = {
                HIMAX_RAW_OUT_SELECT, 0, 0, 0 };
            uint8_t cur[4] = { 0, 0, 0, 0 };
            if (himax_reg_read(_himax, HIMAX_DSRAM_RAW_OUT_SEL, cur, 4) == 0 &&
                    memcmp(cur, raw_out, 4) != 0) {
                trace_f(_probe_fp, "probe native: raw_out_sel=%02x -> %02x\n",
                        cur[0], HIMAX_RAW_OUT_SELECT);
                (void)himax_reg_write(_himax, HIMAX_DSRAM_RAW_OUT_SEL,
                        raw_out, 4);
                usleep(20000);
            }
        }
        /*
         * Give the panel a few tries before concluding it is not streaming:
         * right after a cold boot the touch firmware may still be coming up,
         * and one unlucky read (one that lands across a panel refresh) must
         * not send us down the bring-up path that drops a live IC into
         * SAFE_MODE.
         */
        if (_force_full_bringup) {
            trace_f(_probe_fp, "probe native: forced bring-up after reset\n");
            ok = 0;
        } else {
        trace_f(_probe_fp, "probe native: try live frames first...\n");
        ok = 0;
        for (unsigned attempt = 0; attempt < LIVE_FRAME_TRIES; attempt++) {
            if (himax_read_frame(_himax, _stream) == 0 &&
                    himax_frame_valid(_stream)) {
                ok = 1;
                break;
            }
            if (attempt + 1 < LIVE_FRAME_TRIES)
                usleep(20000);
        }
        if (!ok) {
            /*
             * A cold boot leaves the IC reporting RUNNING but not producing
             * records until it has been through a reset, so reset it once and
             * try again.  This is much gentler than the full bring-up below,
             * which drops a live IC into SAFE_MODE.
             */
            trace_f(_probe_fp, "probe native: no records, resetting IC\n");
            if (_himax->reset)
                _himax->reset(_himax->hw_ctx, true);
            if (_himax->delay_us)
                _himax->delay_us(_himax->hw_ctx, 20000);
            if (_himax->reset)
                _himax->reset(_himax->hw_ctx, false);
            if (_himax->delay_us)
                _himax->delay_us(_himax->hw_ctx, 50000);
            for (unsigned attempt = 0; attempt < LIVE_FRAME_TRIES; attempt++) {
                if (himax_read_frame(_himax, _stream) == 0 &&
                        himax_frame_valid(_stream)) {
                    ok = 1;
                    break;
                }
                if (attempt + 1 < LIVE_FRAME_TRIES)
                    usleep(20000);
            }
            if (ok)
                trace_f(_probe_fp, "probe native: streaming after reset\n");
        }
        }
        if (ok) {
            _himax->ic_id = HIMAX_IC_ID;
            _himax->stage = "live-frame-ok";
            trace_f(_probe_fp, "probe native: IC already streaming, no init\n");
        } else {
            /* Still no records: run the full bring-up.  This board's flash policy
             * never raises the 2nd-flash reload word, so use the same
             * sequence minus that wait; frame reads validate the result. */
            trace_f(_probe_fp, "probe native: chip_detect...\n");
            ok = himax_chip_detect(_himax) == 0;
            trace_f(_probe_fp, "probe native: chip_detect=%d disable_fw...\n", ok);
            ok = ok && himax_disable_fw_reload(_himax) == 0;
            trace_f(_probe_fp, "probe native: disable_fw=%d init...\n", ok);
            ok = ok && himax_power_on_init_skip_reload(_himax, true) == 0;
            trace_f(_probe_fp, "probe native: init=%d\n", ok);
            _force_full_bringup = false;
        }
    } else {
        ok = himax_probe(_himax) == 0;
    }
    if (ok) {
        slog("gaokun3 himaxd: HX%06x ready, raw frame %ux%u\n",
                _himax->ic_id, HIMAX_COLS, HIMAX_ROWS);
        trace_f(_probe_fp, "probe ready ic_id=%06x bus_errors=%u\n",
                _himax->ic_id, _himax->bus_errors);
        return true;
    }
    trace_f(_probe_fp, "probe fail stage=%s bus_errors=%u xfer=%p\n",
            _himax->stage ? _himax->stage : "?", _himax->bus_errors,
            (void*)_himax->bus.xfer);
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
    else if (_probe_failures > PROBE_RETRY_FAST_TRIES)
        next = PROBE_RETRY_MAX_US;
    else if (delay >= PROBE_RETRY_MAX_US / 2U)
        next = PROBE_RETRY_MAX_US;
    else
        next = delay * 2U;
    _probe_delay_us = next;
}

/*
 * Full recovery: hardware-reset the IC and let the next probe re-run the whole
 * bring-up.
 *
 * A plain re-probe is not enough for the failures seen on this board.  The IC
 * can sit in a state where FW_STATUS reads RUNNING and the bus answers, while
 * the raw-report selector has been reset to 0 - the firmware is healthy, the
 * frames are well formed, and yet no capacitance data ever arrives again.
 * Only a real reset plus bring-up clears that; re-probing the same running
 * firmware just reports success and the stream stays dead.
 *
 * This runs only after a sustained failure (see DEAD_ROUNDS_RECOVER), never
 * on a torn read.
 */
static void recover_ic(void) {
    _ready = false;
    _algo.iir_initialized = false;
    _algo.touch_active = false;
    _algo.touch_start_frames = 0;
    memset(_algo.tracks, 0, sizeof(_algo.tracks));
    _read_failures++;
    _recoveries++;
    /* The selector is re-asserted by the bring-up; force the next probe to
     * take the full path rather than trusting a firmware that answers. */
    _raw_sel_check_ms = 0;
    _force_full_bringup = true;
    if (_himax->reset != NULL) {
        _himax->reset(_himax->hw_ctx, true);
        if (_himax->delay_us != NULL)
            _himax->delay_us(_himax->hw_ctx, 20000);
        else
            usleep(20);
        _himax->reset(_himax->hw_ctx, false);
        if (_himax->delay_us != NULL)
            _himax->delay_us(_himax->hw_ctx, 50000);
        else
            usleep(50);
    }
    if (_recoveries <= 3 || (_recoveries & (_recoveries - 1U)) == 0)
        slog("gaokun3 himaxd: no records (%s), full reset + bring-up (%u)\n",
                _himax->stage ? _himax->stage : "?", _recoveries);
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


/*
 * Diagnostic control channel.
 *
 * The counters must never be published from the sampling loop (three
 * synchronous VFS IPCs would sit between two frame reads) and must never be
 * published from the poll/read path either (that is the client's own thread,
 * so the client would wait for the filesystem server to answer).  They are
 * published only when a diagnostic client explicitly asks, via
 *   devcmd /dev/touch0 status
 * which costs the touch path nothing at all.
 */
#define HIMAX_DEV_CMD_STATUS   0x4801

static int touch_dev_cntl(vdevice_t* dev, int from_pid, int cmd,
        proto_t* in, proto_t* ret, void* p) {
    (void)dev; (void)from_pid; (void)in; (void)ret; (void)p;
    if (cmd != HIMAX_DEV_CMD_STATUS)
        return -1;
    write_status();
    return 0;
}

/*
 * Same thing through the string-command path, so the stock tool works:
 *   devcmd /dev/touch0 status
 * Returns the status line itself, which makes the counters reachable even
 * when nothing is polling the device node (and therefore nothing is reading
 * the file that write_status() republishes).
 */
static char* touch_cmd(vdevice_t* dev, int from_pid, int argc, char** argv,
        void* p) {
    char* out;
    (void)dev; (void)from_pid; (void)p;
    /* devcmd sends only the command word: argv[0] is "status", argc 1. */
    if (argc < 1 || strcmp(argv[0], "status") != 0)
        return NULL;
    write_status();
    out = (char*)malloc(512);
    if (out == NULL)
        return NULL;
    snprintf(out, 512,
            "uptime_ms %llu frames %u invalid %u ready %u queue %u "
            "contacts %u presses %u releases %u drops %u duplicates %u torn %u "
            "raw_min %u raw_max %u raw0 %u above_thr %u fps %u busy_pct %u",
            (unsigned long long)kernel_tic_ms(0), _frames, _invalid_frames,
            _ready ? 1U : 0U, _queue_count, _contacts_seen, _presses,
            _releases, _queue_drops, _duplicate_frames, _torn_reads,
            _raw_min, _raw_max, _raw_first, _raw_above_threshold,
            _loop_start_ms ? (unsigned)(_frames * 1000ULL /
                    (kernel_tic_ms(0) - _loop_start_ms)) : 0U,
            _loop_busy_us ? (unsigned)(_loop_busy_us * 100ULL /
                    (kernel_tic_ms(0) - _loop_start_ms) / 1000ULL) : 0U);
    return out;
}

static uint32_t touch_poll(vdevice_t* dev, int fd, int from_pid,
        fsinfo_t* node, void* p) {
    (void)dev; (void)fd; (void)from_pid; (void)node; (void)p;
    /*
     * Nothing but the queue state is touched here.
     *
     * This runs on the IPC thread, which is the same thread that answers the
     * client's read: doing the status file's three synchronous VFS IPCs from
     * here stalls the client's own request for as long as the filesystem
     * server takes, so it must not be published on a timer.  The counters are
     * published on demand instead - see touch_dev_cntl() ("devcmd ... status"), which is
     * driven by a separate diagnostic client, never by the touch client.
     */
    return _queue_count != 0 ? VFS_EVT_RD : 0;
}

static int touch_loop(vdevice_t* dev, void* p) {
    (void)p;
    heart();
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
    keep_raw_out_sel();
    if (!sync_irq())
        _skipped_pulse++;
    bool got_record = false;
    for (unsigned attempt = 0; attempt <= FRAME_RETRY_MAX_EXTRA; attempt++) {
        uint16_t seq = 0;
        if (himax_read_frame_synced(_himax, _stream, HIMAX_STREAM_BYTES,
                    &seq) != 0) {
            /*
             * A read that did not land on a record boundary is a torn read:
             * the event-stack port is a stream, so a panel update overtook the
             * transfer and the 0xa55a header is not at offset 0.  That is
             * normal here and it is NOT a reason to reset the IC - resetting
             * costs a full bring-up (hundreds of milliseconds, and the panel
             * may come back in a state that needs another probe) and throws
             * away every record in the meantime.
             *
             * The port restarts at a record boundary on the next open read, so
             * simply read again: the next record's header is the resync point.
             * Retry immediately - waiting only risks being overtaken again.
             */
            /*
             * Do not reset on a torn read, and do not count it as a reason to.
             * The IC is alive and streaming; only the cut landed in the middle
             * of a record.  The previous version accumulated these into a
             * "the IC must be dead" counter and re-probed, which is how a
             * single busy panel talked a healthy driver into a reset loop
             * (ready 0, no frames, hundreds of ms of bring-up each time).
             *
             * Retry immediately, and keep retrying: each retry costs one
             * record read, and the next record's header is the resync point.
             */
            _torn_reads++;
            continue;
        }
        _frames++;
        if (himax_frame_valid(_stream)) {
            _bad_frames = 0;
            got_record = true;
            /* The sequence word identifies the record: re-reading one the
             * solver has already seen wastes a whole 43 ms transfer. */
            if (seq != 0 && seq == _last_seq) {
                _duplicate_frames++;
                break;
            }
            _last_seq = seq;
            process_frame(dev);
            break;
        }
        _invalid_frames++;
        _bad_frames++;
        /* The marker was where it should be but the grid looked wrong: the
         * payload itself was split.  Let the panel start a fresh record before
         * reading again, otherwise the following read tends to re-cut it. */
        if (attempt < FRAME_RETRY_MAX)
            usleep(FRAME_RETRY_GAP_US);
    }
    /*
     * Recovery is judged on whole iterations, not on individual reads.
     *
     * A torn read says nothing about the IC's health - it says a panel update
     * overtook the transfer - so it must never feed a reset decision.  What
     * does mean the IC is gone is a long run of iterations in which not one
     * usable record was produced, while the bus itself is still answering.
     * Only then is a re-probe worth its cost.
     */
    if (got_record) {
        _silent_rounds = 0;
    } else if (++_silent_rounds >= DEAD_ROUNDS_RECOVER) {
        _silent_rounds = 0;
        recover_ic();
        return 0;
    }
    /* Publish the counters on a fixed cadence whether or not the last record
     * was usable: writing only after a valid frame made a panel stuck in an
     * all-invalid state look identical to a hung daemon. */
    /*
     * The status file is NOT written from this loop.
     *
     * fopen + fputs + fclose is three synchronous IPCs to the filesystem
     * server (sync, FS_CMD_CLOSE, VFS_CLOSE), and EwokOS publishes on close.
     * One slow reply therefore stalls the sampler in the middle of its
     * read/sleep budget: measured at 0.5-1.3 s per write against a 30 ms
     * budget, this daemon stopped sampling entirely for that long and lost
     * every frame in the window.  That was the reported "delay jumps past a
     * second, lots of frames missing" - not the SPI read, the solver, the
     * kernel clock, or the panel.
     *
     * Nothing that talks to another server may sit between two frame reads.
     * The counters accumulate in memory (they are cheap); write_status()
     * copies them out from the polling path, which runs on the IPC thread.
     */
    /* A panel that stops producing valid records is otherwise silent: the
     * counters only moved, nothing was logged.  Say so occasionally. */
    if (_bad_frames >= FRAME_RETRY_MAX + 1U && ((_frames & 255U) == 0U))
        slog("gaokun3 himaxd: %u consecutive unusable frames\n", _bad_frames);
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
    const char* mount = "/dev/touch0";
    /* The GENI (QUP0 SE6) master is the default transport; --gpio selects the
     * GPIO bit-bang master instead, for bring-up and as a fallback. */
    bool native = true, trace = false;
    vdevice_t dev;
    const char* budget_env;
    int i;

    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '/')
            mount = argv[i];
        else if (strcmp(argv[i], "--native") == 0)
            native = true;
        else if (strcmp(argv[i], "--gpio") == 0)
            native = false;
        else if (strcmp(argv[i], "--trace") == 0)
            trace = true;
        else {
            long v = strtol(argv[i], NULL, 10);
            if (v >= FRAME_BUDGET_MIN_MS && v <= FRAME_BUDGET_MAX_MS)
                _frame_budget_ms = (unsigned)v;
            else {
                fprintf(stderr,
                        "usage: himaxd [/dev/touch0] [frame-ms] [--gpio|--native] [--trace]\n");
                return -1;
            }
        }
    }
    budget_env = getenv("HIMAX_FRAME_MS");
    if (budget_env) {
        long v = strtol(budget_env, NULL, 10);
        if (v >= FRAME_BUDGET_MIN_MS && v <= FRAME_BUDGET_MAX_MS)
            _frame_budget_ms = (unsigned)v;
    }
    slog("gaokun3 himaxd: frame budget %u ms\n", _frame_budget_ms);
    _trace_events = trace;
    _trace_fp = fopen("/tmp/touch.log", "w");
    _probe_fp = fopen("/tmp/probe.log", "w");
    _heart_fp = fopen("/tmp/heart.log", "w");
    if (native) {
        if (!himax_native_transport_open())
            return -1;
        _native_probe = true;
    } else if (!himax_gpio_open()) {
        return -1;
    }
    _himax = himax_gpio_dev();
    trace_f(_trace_fp, "pid=%u transport=%s\n",
            (unsigned)getpid(), native ? "native-spi" : "gpio");
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
    dev.dev_cntl = touch_dev_cntl;
    dev.cmd = touch_cmd;
    dev.check_poll_events = touch_poll;
    dev.loop_step = touch_loop;
    {
        int rc = device_run(&dev, mount, FS_TYPE_CHAR, 0444, false);
        fprintf(stderr, "himaxd: device_run rc=%d\n", rc);
        return rc;
    }
}
