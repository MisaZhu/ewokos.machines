/*
 * test_va.c -- host unit test for the GMMU VA free list (ga10b_va.h).
 * Build: cc -I<dir> -Wall -Wextra -o /tmp/test_va <dir>/test/test_va.c
 */
#include <stdio.h>
#include <stdlib.h>
#include "ga10b_va.h"

static int fails;
#define EXPECT(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

#define CAP 8
static uint64_t next;
static ga10b_va_range_t fr[CAP];
static uint32_t n;

static uint64_t take(uint64_t s) { return ga10b_va_take(&next, fr, &n, s); }
static void give(uint64_t va, uint64_t s) { ga10b_va_give(&next, fr, &n, CAP, va, s); }

int main(void) {
    next = 0x1000;
    uint64_t a = take(0x3000), b = take(0x2000), c = take(0x5000);
    EXPECT(a == 0x1000 && b == 0x4000 && c == 0x6000 && next == 0xb000 && n == 0);

    /* a hole is reused, first fit, and split */
    give(b, 0x2000);
    EXPECT(n == 1 && fr[0].va == 0x4000 && fr[0].size == 0x2000 && next == 0xb000);
    uint64_t d = take(0x1000);
    EXPECT(d == 0x4000 && n == 1 && fr[0].va == 0x5000 && fr[0].size == 0x1000);
    uint64_t e = take(0x1000);
    EXPECT(e == 0x5000 && n == 0);
    /* nothing fits: bump */
    give(d, 0x1000);
    uint64_t f = take(0x2000);
    EXPECT(f == 0xb000 && next == 0xd000 && n == 1);

    /* coalescing: prev, next, both */
    give(e, 0x1000);                       /* merges with d's hole */
    EXPECT(n == 1 && fr[0].va == 0x4000 && fr[0].size == 0x2000);
    give(a, 0x3000);                       /* [0x1000,0x4000) joins it */
    EXPECT(n == 1 && fr[0].va == 0x1000 && fr[0].size == 0x5000);

    /* the tail returns to the cursor, pulling free neighbours with it */
    give(f, 0x2000);
    EXPECT(next == 0xb000 && n == 1);
    give(c, 0x5000);
    EXPECT(next == 0x1000 && n == 0);

    /* a daemon lifetime: sessions come and go, the cursor stays bounded */
    uint64_t peak = 0;
    srand(1);
    for (int round = 0; round < 2000; round++) {
        uint64_t v[6], s[6];
        int k = 1 + rand() % 6;
        for (int i = 0; i < k; i++) {
            s[i] = (uint64_t)(1 + rand() % 200) << 12;
            v[i] = take(s[i]);
        }
        if (next > peak)
            peak = next;
        for (int i = 0; i < k; i++) {
            int j = (i * 5 + round) % k;   /* arbitrary free order */
            if (v[j] != ~0ULL) { give(v[j], s[j]); v[j] = ~0ULL; }
        }
        for (int i = 0; i < k; i++)
            if (v[i] != ~0ULL) give(v[i], s[i]);
        EXPECT(next == 0x1000 && n == 0);
    }
    EXPECT(peak <= 0x1000 + (6ull * 200 << 12));

    /* list full: an unmergeable range is dropped, nothing corrupts */
    uint64_t h[2 * CAP + 2];
    for (int i = 0; i < 2 * CAP + 2; i++)
        h[i] = take(0x1000);
    for (int i = 0; i < 2 * CAP + 2; i += 2)
        give(h[i], 0x1000);                /* CAP + 1 isolated holes */
    EXPECT(n == CAP);
    for (uint32_t i = 1; i < n; i++)
        EXPECT(fr[i - 1].va + fr[i - 1].size < fr[i].va);

    printf(fails ? "test_va: %d FAILED\n" : "test_va: all passed\n", fails);
    return fails != 0;
}
