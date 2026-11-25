#ifndef QCOM8GEN3_COUNTER_USEC_H
#define QCOM8GEN3_COUNTER_USEC_H
#include <stdint.h>

/* CNTFRQ is 19.2 MHz on QCOM8GEN3: truncating ticks/usec to 19 speeds up
 * uptime by 1.0526%. Split at whole seconds to retain the fraction without
 * overflowing counter * 1000000 after a few weeks of uptime. */
static inline uint64_t qcom8gen3_counter_usec(uint64_t counter, uint32_t frequency) {
    if (!frequency) return 0;
    return (counter / frequency) * 1000000u +
            ((counter % frequency) * 1000000u) / frequency;
}
#endif
