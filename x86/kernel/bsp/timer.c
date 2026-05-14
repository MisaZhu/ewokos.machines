#include <dev/timer.h>
#include "arch.h"

#define PIT_CH0   0x40
#define PIT_CMD   0x43
#define PIT_INPUT 1193182u

static uint64_t _pit_ticks = 0;
static uint32_t _pit_hz = 100;
static uint32_t _pit_interval_us = 10000;

void timer_init(void) {
	timer_set_interval(0, _pit_hz);
}

void timer_clear_interrupt(uint32_t id) {
	(void)id;
	++_pit_ticks;
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
}

uint64_t timer_read_sys_usec(void) {
	return _pit_ticks * (uint64_t)_pit_interval_us;
}
