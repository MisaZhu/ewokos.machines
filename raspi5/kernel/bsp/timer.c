#include <dev/timer.h>
#include <kernel/irq.h>
#include <kernel/kernel.h>
#include <mm/mmu.h>
#include "timer_arch.h"

/*
 * ARM generic timer (CNTV) as the system tick.
 * On BCM2712 the counter runs at 54MHz (CNTFRQ_EL0),
 * the virtual timer PPI is wired to the GIC-400.
 */

static uint32_t _cntv_step;
static uint32_t _cntv_us_div;

void pi5_dbg_puts(const char* s);

inline void write_cntv_tval(uint32_t tval) {
	__asm__ volatile("msr CNTV_TVAL_EL0, %0" : : "r" (tval) : "memory");
}

static inline uint32_t read_cntfrq(void) {
	uint64_t val;
	__asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r" (val) : : "memory");
	return val;
}

static inline void enable_cntv(void) {
	uint64_t val = 1;
	__asm__ volatile("msr CNTV_CTL_EL0, %0":: "r"(val): "memory");
}

static inline void disable_cntv(void) {
	uint64_t val = 0;
	__asm__ volatile("msr CNTV_CTL_EL0, %0":: "r"(val): "memory");
}

static inline uint64_t read_cntvct(void) {
	uint64_t val;
	__asm__ volatile("mrs %0, CNTVCT_EL0" : "=r" (val) : : "memory");
	return val;
}

void timer_init(void){
	disable_cntv();
	_cntv_us_div = read_cntfrq()/1000000;
	if(_cntv_us_div == 0)
		_cntv_us_div = 1;
	enable_cntv();
}

void timer_clear_interrupt(uint32_t id) {
	(void)id;
	write_cntv_tval(_cntv_step);
}

void timer_set_interval(uint32_t id, uint32_t times_per_sec) {
	(void)id;
	timer_init();
	_cntv_step = read_cntfrq() / times_per_sec;
	timer_clear_interrupt(id);
}

uint64_t timer_read_sys_usec(void) { /*read microsec*/
	if(_cntv_us_div == 0) {
		uint32_t cntfrq = read_cntfrq();
		_cntv_us_div = cntfrq / 1000000U;
		if(_cntv_us_div == 0)
			_cntv_us_div = 1;
	}
	return read_cntvct() / _cntv_us_div;
}
