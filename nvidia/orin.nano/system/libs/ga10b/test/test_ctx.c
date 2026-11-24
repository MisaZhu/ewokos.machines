/*
 * test_ctx.c -- host unit test for the ctxsw header / RAMIN-GR / patch
 * encoders in ga10b_ctx.h.  Expected words derived from nvgpu rel-36:
 *   subctx.c load_ctx_header, ctx.c load_golden_ctx_image,
 *   ramin_gv11b_fusa.c setup_gr, hw_ram_ga10b.h field macros.
 *
 * Build: cc -I<dir> -Wall -Wextra -o /tmp/test_ctx <dir>/test/test_ctx.c
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ga10b_ctx.h"

static int fails;
#define CHECK(expr, want) do { \
    uint32_t _g = (expr), _w = (want); \
    if (_g != _w) { \
        printf("FAIL %s: got 0x%08x want 0x%08x\n", #expr, _g, _w); \
        fails++; \
    } \
} while (0)

int main(void) {
    /* ---- per-veid header page (768B, caller-zeroed) ---- */
    uint32_t h[GA10B_CTXSW_HDR_WORDS];
    memset(h, 0, sizeof(h));
    uint64_t ctx_va   = 0x0000000512345000ULL;
    uint64_t patch_va = 0x0000000180000000ULL;
    uint64_t map_va   = 0x00000002abcd0000ULL;
    ga10b_ctxsw_veid_hdr(h, ctx_va, patch_va, map_va);
    CHECK(h[3],  0x21u);                 /* type_per_veid_header */
    CHECK(h[4],  0u);                    /* patch count */
    CHECK(h[5],  0x80000000u);           /* patch addr RAW lo32 */
    CHECK(h[6],  0x1u);                  /* patch addr hi32 */
    CHECK(h[10], 0u);                    /* pm mode no_ctxsw */
    CHECK(h[11], 0u);
    CHECK(h[37], 0u);
    CHECK(h[30], 0x5u);                  /* ctx buffer ptr RAW hi32 */
    CHECK(h[31], 0x12345000u);           /* ctx buffer ptr RAW lo32 */
    CHECK(h[40], 0u);                    /* golden keeps allow_all */
    CHECK(h[41], 0xabcd0000u);           /* access map RAW lo32 */
    CHECK(h[42], 0x2u);
    CHECK(h[52], 0u); CHECK(h[53], 0u); CHECK(h[61], 0u); CHECK(h[62], 0u);
    /* untouched words stay zero (FECS fills magics at save time) */
    CHECK(h[0], 0u); CHECK(h[1], 0u); CHECK(h[191], 0u);

    /* ---- per-channel main-header patches ---- */
    uint32_t c[GA10B_CTXSW_HDR_WORDS];
    memset(c, 0xa5, sizeof(c));          /* simulated saved golden image */
    ga10b_ctxsw_main_patch(c, patch_va, map_va, 0);
    CHECK(c[52], 0u); CHECK(c[53], 0u); CHECK(c[61], 0u); CHECK(c[62], 0u);
    CHECK(c[40], 2u);                    /* access map use_map */
    CHECK(c[41], 0xabcd0000u);
    CHECK(c[42], 0x2u);
    CHECK(c[15], 0xa5a5a5a5u & ~(1u << 3));  /* verif bit3 cleared only */
    CHECK(c[4],  0u);
    CHECK(c[5],  0x80000000u);
    CHECK(c[6],  0x1u);
    CHECK(c[10], 0u); CHECK(c[11], 0u); CHECK(c[37], 0u);
    CHECK(c[33], 1u);                    /* compute preemption CTA */
    CHECK(c[0],  0xa5a5a5a5u);           /* magics untouched */
    CHECK(c[31], 0xa5a5a5a5u);           /* ctx ptr not re-patched */

    /* ---- RAMIN GR words (golden inst block / channel inst block) ---- */
    uint32_t w[256];
    memset(w, 0, sizeof(w));
    uint64_t pdb_pa = 0x00000004deadb000ULL;
    uint64_t hdr_va = 0x0000000312345000ULL;
    ga10b_ramin_gr(w, pdb_pa, hdr_va);
    /* pdb_lo: ncoh|vol|ver2|big64k | (pa>>12)<<12 -- same as fifo channels */
    CHECK(w[128], 0xc07u | (0xdeadbu << 12));
    CHECK(w[129], 0x4u);
    CHECK(w[131], 0xcafeca11u);
    /* engine_wfi_target: virtual(1<<2) | (va>>12)<<12 */
    CHECK(w[132], 0x4u | (0x12345u << 12));
    CHECK(w[133], 0x3u);
    CHECK(w[134], 0u);                   /* wfi veid */
    CHECK(w[166], 1u);                   /* subctx valid mask: veid0 */
    CHECK(w[167], 0u);
    CHECK(w[168], w[128]);               /* subctx0 pdb == main pdb */
    CHECK(w[169], w[129]);
    CHECK(w[170], 0u);
    CHECK(w[171], 0u);
    CHECK(w[130], 0u);                   /* untouched neighbours */
    CHECK(w[165], 0u);

    /* ---- patch buffer: {addr,data} pairs, 512-slot limit ---- */
    uint32_t p[1024];
    uint32_t cnt = 0;
    CHECK((uint32_t)ga10b_patch_add(p, &cnt, 0x419bd8u, 0x100u), 0u);
    CHECK(cnt, 1u);
    CHECK(p[0], 0x419bd8u);
    CHECK(p[1], 0x100u);
    cnt = GA10B_PATCH_SLOTS;
    CHECK((uint32_t)ga10b_patch_add(p, &cnt, 0, 0), (uint32_t)-1);
    CHECK(cnt, GA10B_PATCH_SLOTS);

    if (fails == 0)
        printf("test_ctx: ALL PASS\n");
    else
        printf("test_ctx: %d FAILURES\n", fails);
    return fails != 0;
}