#ifndef Q8B_COUNTER_USEC_H
#define Q8B_COUNTER_USEC_H
#include <stdint.h>

/* CNTFRQ is 19.2 MHz on Q8B: truncating ticks/usec to 19 speeds up
 * uptime by 1.0526%. Split at whole seconds to retain the fraction without
 * overflowing counter * 1000000 after a few weeks of uptime. */
static inline uint64_t q8b_counter_usec(uint64_t counter, uint32_t frequency) {
    if (!frequency) return 0;
    return (counter / frequency) * 1000000u +
            ((counter % frequency) * 1000000u) / frequency;
}
#endif
