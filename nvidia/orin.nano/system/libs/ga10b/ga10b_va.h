/*
 * ga10b_va.h - GPU VA allocation for the bump-mapped (persistent) ranges of
 * a GMMU: a cursor plus a free list of returned ranges, sorted by address
 * and coalesced.  Reusing a returned range also reuses its page tables, so
 * the table pool tracks the peak mapped footprint instead of everything a
 * long-lived owner (the resident GPU daemon) ever mapped.
 * Pure data structure, no MMIO: shared with the host unit test.
 */
#ifndef GA10B_VA_H
#define GA10B_VA_H

#include <stdint.h>
#include <string.h>

typedef struct {
    uint64_t va;
    uint64_t size;
} ga10b_va_range_t;

/* first fit from the free list, else the bump cursor */
static inline uint64_t ga10b_va_take(uint64_t* next, ga10b_va_range_t* fr,
                                     uint32_t* n, uint64_t size) {
    for (uint32_t i = 0; i < *n; i++) {
        if (fr[i].size < size)
            continue;
        uint64_t va = fr[i].va;
        fr[i].va += size;
        fr[i].size -= size;
        if (fr[i].size == 0) {
            memmove(&fr[i], &fr[i + 1], (*n - i - 1) * sizeof(*fr));
            (*n)--;
        }
        return va;
    }
    uint64_t va = *next;
    *next += size;
    return va;
}

/* return [va, va+size).  A range ending at the cursor pulls the cursor back;
 * with the list full a range that cannot merge is dropped (its VA is lost,
 * as before this allocator existed). */
static inline void ga10b_va_give(uint64_t* next, ga10b_va_range_t* fr,
                                 uint32_t* n, uint32_t cap, uint64_t va,
                                 uint64_t size) {
    if (size == 0)
        return;
    uint32_t i = 0;
    while (i < *n && fr[i].va < va)
        i++;
    int prev = i > 0 && fr[i - 1].va + fr[i - 1].size == va;
    int nxt = i < *n && va + size == fr[i].va;
    if (prev) {
        fr[i - 1].size += size;
        if (nxt) {
            fr[i - 1].size += fr[i].size;
            memmove(&fr[i], &fr[i + 1], (*n - i - 1) * sizeof(*fr));
            (*n)--;
        }
    } else if (nxt) {
        fr[i].va = va;
        fr[i].size += size;
    } else if (va + size == *next) {
        *next = va;
    } else if (*n < cap) {
        memmove(&fr[i + 1], &fr[i], (*n - i) * sizeof(*fr));
        fr[i].va = va;
        fr[i].size = size;
        (*n)++;
    }
    while (*n > 0 && fr[*n - 1].va + fr[*n - 1].size == *next) {
        *next = fr[*n - 1].va;
        (*n)--;
    }
}

#endif
