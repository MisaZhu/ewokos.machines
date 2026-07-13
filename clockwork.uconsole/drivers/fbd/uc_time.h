#ifndef UC_TIME_H
#define UC_TIME_H

#include <stdint.h>

/*
 * BCM2711 System Timer (STC) helpers. The 1 MHz free-running counter at
 * MMIO+0x3004 gives us a reliable microsecond time base — the kernel's
 * SYS_USLEEP is timer-tick granular (~1 ms with timer_freq=1024), which
 * is too coarse for the OCP8178 single-wire protocol.
 *
 * uc_time_init() must be called after mmio_map() has populated _mmio_base.
 */

void     uc_time_init(void);
uint32_t uc_micros(void);          /* free-running μs since power-on */
void     uc_udelay(uint32_t us);   /* busy-wait μs */
void     uc_mdelay(uint32_t ms);   /* usleep-based ms wait */

#endif
