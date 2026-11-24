/*
 * test_mm.c -- host unit test for the GMMU ver2 PDE/PTE encoders in
 * ga10b_mm.h.  Expected words derived from nvgpu rel-36
 * gmmu_ga10b_fusa.c + hw_gmmu_ga10b.h field macros (sys_ncoh + vol).
 *
 * Build: cc -I<dir> -Wall -Wextra -o /tmp/test_mm <dir>/test/test_mm.c
 */
#include <stdint.h>
#include <stdio.h>
#include "ga10b_mm.h"

static int fails;
#define CHECK(expr, want) do { \
    uint64_t _g = (expr), _w = (want); \
    if (_g != _w) { \
        printf("FAIL %s: got 0x%llx want 0x%llx\n", #expr, \
               (unsigned long long)_g, (unsigned long long)_w); \
        fails++; \
    } \
} while (0)

int main(void) {
    uint32_t e[4];

    /* PDE3/2/1: w0 = ncoh(6)|vol(8)|((pa>>12 & 0xffffff)<<8), w1 = pa>>36 */
    ga10b_gmmu_pde(e, 0x123456000ULL, GA10B_GMMU_PDE_APERTURE_NCOH);
    CHECK(e[0], 0x1234560eu);
    CHECK(e[1], 0x00000000u);
    /* address above 2^36: w1 carries pa>>36 */
    ga10b_gmmu_pde(e, 0x1234567000ULL, GA10B_GMMU_PDE_APERTURE_NCOH);
    CHECK(e[0], 0x2345670eu);
    CHECK(e[1], 0x00000001u);
    /* 40-bit address: pa>>12 = 0xff00000 -> lo24 = 0xf00000, hi = 0xf */
    ga10b_gmmu_pde(e, 0xff00000000ULL, GA10B_GMMU_PDE_APERTURE_NCOH);
    CHECK(e[0], 0xf000000eu);
    CHECK(e[1], 0x0000000fu);

    /* PDE0 small half: w0/w1 (big) invalid, w2/w3 = PDE formula */
    ga10b_gmmu_pde0_small(e, 0x123456000ULL, GA10B_GMMU_PDE_APERTURE_NCOH);
    CHECK(e[0], 0u);
    CHECK(e[1], 0u);
    CHECK(e[2], 0x1234560eu);
    CHECK(e[3], 0x00000000u);

    /* PTE: valid(1)|ncoh(6)|vol(8) = 0x0f (no priv: nvgpu flag_none), pa>>12 << 8 */
    ga10b_gmmu_pte(e, 0xabcde000ULL, GA10B_GMMU_PTE_APERTURE_NCOH,
                   GA10B_GMMU_PTE_VOL);
    CHECK(e[0], 0x0abcde0fu);
    CHECK(e[1], 0x00000000u);
    ga10b_gmmu_pte(e, 0xff00000000ULL, GA10B_GMMU_PTE_APERTURE_NCOH,
                   GA10B_GMMU_PTE_VOL);
    CHECK(e[0], 0xf000000fu);
    CHECK(e[1], 0x0000000fu);

    /* decode round-trip */
    ga10b_gmmu_pde(e, 0x123456000ULL, GA10B_GMMU_PDE_APERTURE_NCOH);
    CHECK(ga10b_gmmu_pde_pa(e[0], e[1]), 0x123456000ULL);
    ga10b_gmmu_pde(e, 0xff00000000ULL, GA10B_GMMU_PDE_APERTURE_NCOH);
    CHECK(ga10b_gmmu_pde_pa(e[0], e[1]), 0xff00000000ULL);
    CHECK(ga10b_gmmu_pde_valid(0x1234560eu), 1);
    CHECK(ga10b_gmmu_pde_valid(0), 0);

    if (fails == 0) {
        printf("test_mm: ALL PASS\n");
        return 0;
    }
    printf("test_mm: %d FAIL\n", fails);
    return 1;
}