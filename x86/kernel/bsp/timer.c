#include <dev/timer.h>
#include "arch.h"

#define PIT_CH0   0x40
#define PIT_CMD   0x43
#define PIT_INPUT 1193182u

static uint64_t _pit_ticks = 0;
static uint32_t _pit_hz = 100;
static uint32_t _pit_interval_us = 10000;

/* 0 = not calibrated yet: timer_fine_cnt() then reports no user-readable counter. */
static uint32_t _tsc_hz = 0;

/*
 * Interrupt-driven TSC calibration against PIT channel 0 (the system tick).
 * The channel-2/speaker-gate countdown is unreliable under some hypervisors
 * (its terminal-count output reads high immediately, so the measured window
 * collapses to zero), but channel 0 demonstrably works - it drives the kernel
 * tick. So instead of a blocking one-shot measurement we accumulate rdtsc
 * across channel-0 interrupts in timer_clear_interrupt().
 *
 * One window is NOT enough: while interrupts are masked under heavy boot/init
 * load, PIT edges are lost, _pit_ticks under-counts, and the same tick count
 * then spans MORE real time - inflating the derived frequency. Lost edges can
 * only ever inflate a window, so we measure MANY SHORT non-overlapping windows
 * over a bounded phase and keep the MINIMUM. Short windows matter: tick loss is
 * sporadic, so a brief lull yields a clean (edge-loss-free) window whose rate is
 * the true one, and the more windows we sample the likelier we catch one. Long
 * windows instead average over busy and idle stretches and stay contaminated
 * high. The minimum is the right selector because every error source here -
 * lost edges - only pushes a window up, never down.
 *
 * The phase restarts whenever timer_set_interval() changes the tick rate, so a
 * window always covers a single stable interval. If no plausible value is ever
 * measured, _tsc_hz stays 0, timer_fine_cnt() reports hz == 0, and libc keeps
 * the tick-quantized clock exactly as before - never a wrong clock.
 */
#define TSC_CAL_WINDOW_US 50000u     /* one measurement window ~50ms of ticks */
#define TSC_CAL_PHASE_US  3000000u   /* keep best-of-windows for ~3s, then freeze */

static int      _cal_done = 0;       /* 1 once the phase ended and _tsc_hz is final */
static int      _cal_started = 0;    /* 1 once the current window's first tick is captured */
static uint64_t _cal_tsc0 = 0;       /* rdtsc at the current window's first tick */
static uint64_t _cal_tick0 = 0;      /* _pit_ticks at the current window's first tick */
static uint32_t _cal_hz = 0;         /* _pit_hz frozen for the whole phase */
static uint64_t _cal_phase_end = 0;  /* _pit_ticks at which the phase freezes */
static uint64_t _cal_best = 0;       /* min hz across windows so far (0 = none yet) */

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void timer_init(void) {
    timer_set_interval(0, _pit_hz);
}

void timer_clear_interrupt(uint32_t id) {
    (void)id;
    ++_pit_ticks;

    if(_cal_done)
        return;

    uint64_t tsc_now = rdtsc();
    if(!_cal_started) {
        /* first tick of the phase: anchor tsc/tick, freeze the rate, set the deadline */
        _cal_started   = 1;
        _cal_tsc0      = tsc_now;
        _cal_tick0     = _pit_ticks;
        _cal_hz        = _pit_hz;
        _cal_phase_end = _pit_ticks +
            ((uint64_t)_cal_hz * TSC_CAL_PHASE_US / 1000000u);
        return;
    }

    uint64_t dticks = _pit_ticks - _cal_tick0;
    /* ticks needed to span ~TSC_CAL_WINDOW_US at the frozen rate (>=1) */
    uint64_t target = (uint64_t)_cal_hz * TSC_CAL_WINDOW_US / 1000000u;
    if(target == 0)
        target = 1;

    if(dticks >= target) {
        /* hz = dtsc / (dticks / _cal_hz) = dtsc * _cal_hz / dticks */
        uint64_t dtsc = tsc_now - _cal_tsc0;
        uint64_t hz = dtsc * (uint64_t)_cal_hz / dticks;
        /*
         * timer_fine_cnt() returns uint32_t, so reject anything that would
         * truncate or is physically implausible (< 1MHz or > 4.29GHz). Keep the
         * minimum plausible value: lost PIT edges only ever inflate a window.
         */
        if(hz >= 1000000ULL && hz <= 0xffffffffULL) {
            if(_cal_best == 0 || hz < _cal_best) {
                _cal_best = hz;
                _tsc_hz = (uint32_t)hz;   /* best (lowest) estimate so far */
            }
        }
        /* slide to a fresh non-overlapping window */
        _cal_tsc0  = tsc_now;
        _cal_tick0 = _pit_ticks;
    }

    if(_pit_ticks >= _cal_phase_end)
        _cal_done = 1;   /* freeze the best estimate for the rest of uptime */
}

void timer_set_interval(uint32_t id, uint32_t times_per_sec) {
    uint16_t divisor;
    (void)id;
    if (times_per_sec == 0) {
        times_per_sec = 100;
    }
    _pit_hz = times_per_sec;
    _pit_interval_us = 1000000u / _pit_hz;
    divisor = (uint16_t)(PIT_INPUT / _pit_hz);
    outb(PIT_CMD, 0x36);
    outb(PIT_CH0, divisor & 0xFF);
    outb(PIT_CH0, divisor >> 8);

    /* tick rate changed: restart the whole calibration phase on the new rate */
    if(!_cal_done) {
        _cal_started = 0;
        _cal_best = 0;
        _tsc_hz = 0;
    }
}

uint64_t timer_read_sys_usec(void) {
    return _pit_ticks * (uint64_t)_pit_interval_us;
}

/*
 * <dev/timer.h>: override the platform's weak no-counter stub with the calibrated
 * TSC. The counter returned here is the very one userspace reads with rdtsc (see
 * libewoksys kernel_tic.c fine_cnt_read()), which is the contract the vsyscall
 * clock interpolation depends on. Returns hz == 0 until calibration yields a
 * plausible rate, so libc falls back to the tick-quantized clock rather than
 * interpolating from a counter it cannot scale to nanoseconds.
 */
uint32_t timer_fine_cnt(uint64_t* cnt) {
    if(_tsc_hz == 0)
        return 0;
    if(cnt)
        *cnt = rdtsc();
    return _tsc_hz;
}
