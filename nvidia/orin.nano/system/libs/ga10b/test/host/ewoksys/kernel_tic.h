/* host shim: no fine clock (the spin path is hardware-only) */
#ifndef HOST_KERNEL_TIC_H
#define HOST_KERNEL_TIC_H
#include <stdint.h>
static inline int32_t kernel_tic_nsec(uint64_t* nsec) { *nsec = 0; return 0; }
static inline int32_t kernel_tic(uint32_t* sec, uint64_t* usec) {
    if (sec) *sec = 0;
    if (usec) *usec = 0;
    return 0;
}
#endif
