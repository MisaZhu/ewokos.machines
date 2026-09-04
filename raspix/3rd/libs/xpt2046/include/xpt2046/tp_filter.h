#ifndef XPT2046_TP_FILTER_H
#define XPT2046_TP_FILTER_H

#include <stdint.h>

/* Touch position filtering pipeline (classic combo):
 *   raw ADC -> clamp (reject jumps) -> median (kill impulses)
 *           -> moving average (smooth) -> output
 * Filters are re-seeded on every pen-down edge so a new touch never
 * inherits the previous touch's coordinates; on release the last
 * filtered position is reported. */

#ifndef TP_MAX_JUMP
#define TP_MAX_JUMP  400 /* max plausible delta between two samples (~1/10 of the 12-bit range) */
#endif

#ifndef TP_MEDIAN_N
#define TP_MEDIAN_N  5   /* median window (odd) */
#endif

#ifndef TP_AVG_N
#define TP_AVG_N     4   /* moving average window */
#endif

typedef struct {
    uint16_t med[TP_MEDIAN_N];
    uint8_t  med_n;   /* samples filled in the median window */
    uint8_t  med_idx; /* median ring write position */
    uint16_t avg[TP_AVG_N];
    uint8_t  avg_n;
    uint8_t  avg_idx;
    uint16_t last;    /* last accepted (clamped) sample */
    uint16_t out;     /* last filtered output */
    uint8_t  seeded;  /* clamp reference valid */
} tp_filter_t;

void tp_filter_reset(tp_filter_t* f);
uint16_t tp_filter_in(tp_filter_t* f, uint16_t v);

#endif
