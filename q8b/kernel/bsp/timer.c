#include <dev/timer.h>
#include <kernel/irq.h>
#include <kernel/kernel.h>
#include <mm/mmu.h>
#include "hw_arch.h"
#include <q8b_counter_usec.h>

/*
 * ARM generic virtual timer (CNTV) as the EwokOS system tick.
 * The counter frequency is the firmware-programmed CNTFRQ_EL0; the virtual
 * timer PPI is INTID 27 on the Q8B GICv3.
 */

static uint32_t _cntv_step;
static uint32_t _cntv_hz;

void write_cntv_tval(uint32_t tval) {
    __asm__ volatile("msr CNTV_TVAL_EL0, %0" : : "r"(tval) : "memory");
}

static inline uint32_t read_cntfrq(void) {
    uint64_t val;
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(val) : : "memory");
    return (uint32_t)val;
}

static inline void enable_cntv(void) {
    uint64_t val = 1;
    __asm__ volatile("msr CNTV_CTL_EL0, %0" :: "r"(val) : "memory");
}

static inline void disable_cntv(void) {
    uint64_t val = 0;
    __asm__ volatile("msr CNTV_CTL_EL0, %0" :: "r"(val) : "memory");
}

void timer_disable_local(void) {
    disable_cntv();
}

static inline uint64_t read_cntvct(void) {
    uint64_t val;
    __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(val) : : "memory");
    return val;
}

void timer_enable_el0_cnt_access(void) {
    /* Preserve the event-stream controls used by WFE-based SMP waiters. */
    uint64_t val;
    __asm__ volatile("mrs %0, CNTKCTL_EL1" : "=r"(val) : : "memory");
    val |= 0x3; /* EL0PCTEN | EL0VCTEN */
    __asm__ volatile("msr CNTKCTL_EL1, %0" :: "r"(val) : "memory");
    __asm__ volatile("isb" ::: "memory");
}

void timer_init(void) {
    timer_enable_el0_cnt_access();
    disable_cntv();
    _cntv_hz = read_cntfrq();
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

uint64_t timer_read_sys_usec(void) {
    if (_cntv_hz == 0)
        _cntv_hz = read_cntfrq();
    return q8b_counter_usec(read_cntvct(), _cntv_hz);
}
