#include <xpt2046/tp_filter.h>
#include <string.h>

void tp_filter_reset(tp_filter_t* f) {
    f->med_n = f->med_idx = 0;
    f->avg_n = f->avg_idx = 0;
    f->seeded = 0;
    /* keep f->out: release events still report the last filtered position */
}

uint16_t tp_filter_in(tp_filter_t* f, uint16_t v) {
    uint8_t i;

    /* 1. clamp: reject samples jumping too far from the last accepted one */
    if(f->seeded) {
        int32_t d = (int32_t)v - (int32_t)f->last;
        if(d > TP_MAX_JUMP || d < -TP_MAX_JUMP)
            v = f->last;
    }
    f->last = v;

    /* 2. median over the clamped window (kills impulse noise) */
    f->med[f->med_idx] = v;
    f->med_idx = (f->med_idx + 1) % TP_MEDIAN_N;
    if(f->med_n < TP_MEDIAN_N)
        f->med_n++;

    uint16_t tmp[TP_MEDIAN_N];
    memcpy(tmp, f->med, f->med_n * sizeof(uint16_t));
    for(i = 1; i < f->med_n; i++) { /* insertion sort, N is tiny */
        uint16_t k = tmp[i];
        int8_t j = (int8_t)i - 1;
        while(j >= 0 && tmp[j] > k) {
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = k;
    }
    uint16_t m = tmp[f->med_n / 2];

    /* 3. moving average over the median outputs (smoothing) */
    f->avg[f->avg_idx] = m;
    f->avg_idx = (f->avg_idx + 1) % TP_AVG_N;
    if(f->avg_n < TP_AVG_N)
        f->avg_n++;

    uint32_t s = 0;
    for(i = 0; i < f->avg_n; i++)
        s += f->avg[i];
    f->out = (uint16_t)(s / f->avg_n);
    f->seeded = 1;
    return f->out;
}
