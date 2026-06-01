#include <mm/mmu.h>
#include <dev/timer.h>
#include <kernel/irq.h>
#include <kernel/kernel.h>
#include <kprintf.h>
#include "timer_arch.h"

#define GIC_DEFAULT_FREQ	6000000

uint32_t _timer_tval = 0;
static uint32_t _cntfrq = GIC_DEFAULT_FREQ;

static inline uint32_t read_cntfrq(void) {
  uint32_t val;
  __asm__ volatile ("mrc p15, 0, %0, c14, c0, 0" : "=r"(val) );
  return val;
}

static inline uint64_t read_cntpct(void) {
	uint64_t val;
	__asm__ volatile("mrrc p15, 0, %Q0, %R0, c14" : "=r" (val));
	return val;
}

inline void write_cntv_tval(uint32_t tval) {
	__asm__ volatile ("mcr p15, 0, %0, c14, c3, 0" :: "r"(tval));
}

static inline uint32_t read_cntv_tval(void) {
	uint32_t val;
	__asm__ volatile ("mrc p15, 0, %0, c14, c3, 0" : "=r"(val));
	return val;
}

static inline void enable_cntv(void) {
	__asm__ volatile ("mcr p15, 0, %0, c14, c3, 1" :: "r"(1));
	__asm__ volatile ("isb");
}

static inline void disable_cntv(void) {
	__asm__ volatile ("mcr p15, 0, %0, c14, c3, 1" :: "r"(0));
	__asm__ volatile ("isb");
}

static inline uint64_t  read_cntvct(void) {
	uint64_t val;
	__asm__ volatile("mrrc p15, 1, %Q0, %R0, c14" : "=r" (val));
	return val;
}

static inline uint32_t read_cntctl(void) {
	uint32_t val;
	__asm__ volatile("mrc p15, 0, %0, c14, C3, 1" : "=r" (val));
	return val;
}

static inline uint32_t timer_sanitize_cntfrq(uint32_t freq) {
	if(freq < 1000000 || freq > 50000000)
		return GIC_DEFAULT_FREQ;
	return freq;
}

void timer_init(void) {
	_cntfrq = timer_sanitize_cntfrq(read_cntfrq());
}

void timer_set_interval(uint32_t id, uint32_t times_per_sec) {
	(void)id;
	if(times_per_sec == 0)
		return;

	_cntfrq = timer_sanitize_cntfrq(read_cntfrq());
	_timer_tval = _cntfrq / times_per_sec;
	if(_timer_tval == 0)
		_timer_tval = 1;

	disable_cntv();
	write_cntv_tval(_timer_tval);
	enable_cntv();
}

inline void timer_clear_interrupt(uint32_t id) {
	(void)id;
	if(_timer_tval != 0)
		write_cntv_tval(_timer_tval);
}

inline uint64_t timer_read_sys_usec(void) { //read microsec
	uint32_t freq = timer_sanitize_cntfrq(read_cntfrq());
	_cntfrq = freq;

	/*
	 * Keep the runtime clock conversion consistent with the effective counter
	 * frequency chosen in timer_set_interval(). The previous fixed /6MHz path
	 * made Miyoo's proc runtime window drift badly when CNTFRQ was not exactly
	 * 6MHz, which showed up as xprocs/xcores reporting K:100%.
	 */
	return (read_cntvct() * 1000000ULL) / freq;
}
