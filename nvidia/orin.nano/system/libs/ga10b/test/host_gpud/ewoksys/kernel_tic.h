/* host shim: a clock the test advances by hand (one step per stub launch) */
#ifndef HOST_GPUD_KERNEL_TIC_H
#define HOST_GPUD_KERNEL_TIC_H
#include <stdint.h>
extern uint64_t g_fake_nsec;
static inline int32_t kernel_tic_nsec(uint64_t* nsec) { *nsec = g_fake_nsec; return 0; }
#endif
