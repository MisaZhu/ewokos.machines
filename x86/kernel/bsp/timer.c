#include <dev/timer.h>
#include <stdint.h>
#include "arch.h"

#define PIT_CH0   0x40
#define PIT_CMD   0x43
#define PIT_INPUT 1193182u

/* 校准长度: 64 个 PIT 周期 ≈ 62ms, TSC 频率分辨率 ~1.5% */
#define TSC_CAL_TICKS 64

static uint64_t _pit_ticks = 0;
static uint32_t _pit_hz = 100;
static uint32_t _pit_interval_us = 10000;

/*
 * 时间源 = TSC, 由 PIT 校准一次。
 *
 * 旧实现的时间是纯软件计数 (_pit_ticks * interval), 只在 core0 的 tick
 * 中断里推进。真机上 PIT 中断会被成批丢弃: 内核 IF=0 窗口超过 tick 周期
 * (976us@1024Hz) 时边沿触发的中断直接丢失, 固件 SMI 也会整段偷走时间
 * (QEMU 两者皆无)。时间一停, 所有 SLEEPING 进程的 sleep_counter 永不
 * 减少 —— usleep 驱动的轮询驱动 (usbhostd -> 键鼠) 与帧步进 (xserverd)
 * 全部冻死, 当前任务也失去抢占。
 *
 * Intel Core 世代之后 TSC 为 invariant (恒定频率, 不随节能变频, 不受
 * IF/SMI 影响, 各核同源), 读一次几十个周期。这里在编完 PIT 后用 PIT
 * 本身校准 TSC 频率, 之后 timer_read_sys_usec 完全由 TSC 推进; PIT
 * 中断只保留调度抢占一个职责, _pit_ticks 仅用于丢 tick 对账诊断。
 */
static uint64_t _tsc_base = 0;      /* 校准结束时刻的 TSC 读数 */
static uint64_t _tsc_base_usec = 0; /* 该时刻已流过的软件时间 */
static uint64_t _tsc_counts_per_usec = 0; /* 校准结果, 0 = 尚未校准 */

static inline uint64_t tsc_read(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* 锁存并读取 ch0 当前计数值 (mode 2: N..1 循环) */
static uint16_t pit_tc_read(void) {
    uint16_t tc;
    outb(PIT_CMD, 0x00); /* ch0 latch 命令 */
    tc = (uint16_t)(inb(PIT_CH0));
    tc |= (uint16_t)((uint16_t)inb(PIT_CH0) << 8);
    return tc;
}

static void tsc_calibrate(void) {
    uint64_t t0, t1;
    uint16_t last, cur;
    uint32_t wraps = 0;

    last = pit_tc_read();
    t0 = tsc_read();
    while(wraps < TSC_CAL_TICKS) {
        cur = pit_tc_read();
        if(cur > last) /* 计数穿过 1 回卷到 N: 走完一个周期 */
            wraps++;
        last = cur;
    }
    t1 = tsc_read();

    uint64_t delta = t1 - t0;
    if(delta == 0 || wraps == 0)
        return;
    uint64_t counts_per_usec = ((delta / wraps) * (uint64_t)_pit_hz) / 1000000ULL;
    if(counts_per_usec == 0) /* TSC < 1MHz 视为校准失败, 回退软件计数 */
        return;

    _tsc_counts_per_usec = counts_per_usec;
    _tsc_base = t1;
    _tsc_base_usec = _pit_ticks * (uint64_t)_pit_interval_us;
}

void timer_init(void) {
    timer_set_interval(0, _pit_hz);
}

void timer_clear_interrupt(uint32_t id) {
    (void)id;
    ++_pit_ticks; /* 仅供丢 tick 诊断对账, 不再承载时间 */
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
    outb(PIT_CMD, 0x34); /* ch0, mode 2 (rate generator), 16bit */
    outb(PIT_CH0, divisor & 0xFF);
    outb(PIT_CH0, divisor >> 8);
    tsc_calibrate();
}

uint64_t timer_read_sys_usec(void) {
    if(_tsc_counts_per_usec == 0)
        return _pit_ticks * (uint64_t)_pit_interval_us;

    uint64_t t = tsc_read();
    if(t <= _tsc_base)
        return _tsc_base_usec;
    return _tsc_base_usec + (t - _tsc_base) / _tsc_counts_per_usec;
}

/*
 * <dev/timer.h>: override the platform's weak no-counter stub with the calibrated
 * TSC. The counter returned here is the very one userspace reads with rdtsc (see
 * libewoksys kernel_tic.c fine_cnt_read()), which is the contract the vsyscall
 * clock interpolation depends on - and the very TSC timer_read_sys_usec() runs
 * on. Returns hz == 0 until the one-shot tsc_calibrate() yields a plausible
 * rate, so libc falls back to the tick-quantized clock rather than
 * interpolating from a counter it cannot scale to nanoseconds.
 */
uint32_t timer_fine_cnt(uint64_t* cnt) {
    uint64_t hz;
    if(_tsc_counts_per_usec == 0)
        return 0;
    hz = _tsc_counts_per_usec * 1000000ULL;
    if(hz > 0xffffffffULL) /* 契约返回 uint32; TSC > ~4.29GHz 视为不可表示 */
        return 0;
    if(cnt)
        *cnt = tsc_read();
    return (uint32_t)hz;
}
