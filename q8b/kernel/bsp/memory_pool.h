#ifndef Q8B_MEMORY_POOL_H
#define Q8B_MEMORY_POOL_H

#include <stdint.h>

typedef struct { uint64_t base, top; } q8b_mem_range_t;

/* Grow through touching reclaimable descriptors, independent of map order. */
static inline uint64_t q8b_pool_extend(uint64_t top, uint32_t count,
        int32_t (*read_range)(uint32_t, uint64_t*, uint64_t*)) {
    uint64_t previous;
    do {
        previous = top;
        for (uint32_t i = 0; i < count; i++) {
            uint64_t b, t;
            if (read_range(i, &b, &t) && b <= top && t > top)
                top = t;
        }
    } while (top != previous);
    return top;
}

/* Find a page-aligned pool wholly inside a reclaimable physical interval.
 * Exclusions may be unsorted/overlapping and are rounded outwards by
 * advancing the candidate to the next complete CPU page. Zero means none. */
static inline uint64_t q8b_pool_candidate(uint64_t base, uint64_t top,
        uint64_t size, uint64_t page, const q8b_mem_range_t* ex, uint32_t n) {
    if (!size || !page || (page & (page - 1)) || (size & (page - 1)))
        return 0;
    for (;;) {
        if (base > UINT64_MAX - (page - 1))
            return 0;
        base = (base + page - 1) & ~(page - 1);
        if (base >= top || size > top - base)
            return 0;
        uint64_t next = base;
        for (uint32_t i = 0; i < n; i++) {
            if (ex[i].top > ex[i].base && ex[i].base < base + size &&
                    ex[i].top > base && ex[i].top > next)
                next = ex[i].top;
        }
        if (next == base)
            return base;
        base = next;
    }
}
#endif
