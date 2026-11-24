/*
 * test_netlist.c -- host unit test for the NETC container parser
 * (ga10b_fw_netlist_find) + typed accessors (ga10b_netlist.h).
 *
 * Synthetic containers exercise every bounds/alignment rejection path;
 * the real NETC blob (NOT in the repo) is checked when a path is given:
 *   /tmp/test_netlist /Users/tony/work/gpu-fw/ga10b/NETC_img_prod_encrypted.bin
 *
 * Build: cc -I<dir> -Wall -Wextra -o /tmp/test_netlist \
 *        <dir>/test/test_netlist.c <dir>/ga10b_fw_parse.c
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ga10b_netlist.h"

static int fails;
#define EXPECT(cond) do { \
    if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } \
} while (0)

/* synthetic container: 3 regions (MAJORV u32, SW_CTX_LOAD aiv x2,
 * SW_BUNDLE_INIT av x2), table = 2 + 3*3 = 11 words */
static uint32_t synth[64];
static uint32_t synth_size(void) {
    memset(synth, 0, sizeof(synth));
    synth[0] = 0;                       /* version */
    synth[1] = 3;                       /* regions */
    /* {id, size, offset} */
    synth[2] = GA10B_NETC_MAJORV; synth[3] = 4;  synth[4] = 44;
    synth[5] = GA10B_NETC_SW_CTX_LOAD; synth[6] = 24; synth[7] = 48;
    synth[8] = GA10B_NETC_SW_BUNDLE_INIT; synth[9] = 16; synth[10] = 72;
    synth[11] = 0x01020304u;            /* MAJORV */
    /* aiv entries: {addr, index, value} */
    synth[12] = 0x00419000u; synth[13] = 7; synth[14] = 0x11111111u;
    synth[15] = 0x00504000u; synth[16] = 0; synth[17] = 0x22222222u;
    /* av entries */
    synth[18] = 0x00419bd8u; synth[19] = 0x100u;
    synth[20] = 0x0000e100u; synth[21] = 0x1u;
    return 88;                          /* bytes */
}

static void test_synthetic(void) {
    uint32_t sz = synth_size();
    uint32_t off, rsz, val, a, i, v, lo, hi;

    EXPECT(ga10b_fw_netlist_find(synth, sz, GA10B_NETC_MAJORV,
                                 &off, &rsz) == 0);
    EXPECT(off == 44 && rsz == 4);
    EXPECT(ga10b_netlist_u32(synth, sz, GA10B_NETC_MAJORV, &val) == 0);
    EXPECT(val == 0x01020304u);

    EXPECT(ga10b_netlist_count(synth, sz, GA10B_NETC_SW_CTX_LOAD, 3) == 2);
    EXPECT(ga10b_netlist_aiv(synth, sz, GA10B_NETC_SW_CTX_LOAD, 1,
                             &a, &i, &v) == 0);
    EXPECT(a == 0x00504000u && i == 0 && v == 0x22222222u);
    EXPECT(ga10b_netlist_aiv(synth, sz, GA10B_NETC_SW_CTX_LOAD, 2,
                             &a, &i, &v) == -1);

    EXPECT(ga10b_netlist_count(synth, sz, GA10B_NETC_SW_BUNDLE_INIT, 2) == 2);
    EXPECT(ga10b_netlist_av(synth, sz, GA10B_NETC_SW_BUNDLE_INIT, 0,
                            &a, &v) == 0);
    EXPECT(a == 0x00419bd8u && v == 0x100u);
    EXPECT(ga10b_netlist_av(synth, sz, GA10B_NETC_SW_BUNDLE_INIT, 2,
                            &a, &v) == -1);

    /* absent region: count 0, accessors fail */
    EXPECT(ga10b_netlist_count(synth, sz, GA10B_NETC_SW_BUNDLE64_INIT, 3) == 0);
    EXPECT(ga10b_netlist_av64(synth, sz, GA10B_NETC_SW_BUNDLE64_INIT, 0,
                              &a, &lo, &hi) == -1);
    /* u32 accessor rejects multi-word regions */
    EXPECT(ga10b_netlist_u32(synth, sz, GA10B_NETC_SW_CTX_LOAD, &val) == -1);

    /* container rejections */
    synth[0] = 1;                       /* bad version */
    EXPECT(ga10b_fw_netlist_find(synth, sz, GA10B_NETC_MAJORV,
                                 &off, &rsz) == -1);
    synth[0] = 0;
    synth[1] = 0;                       /* zero regions */
    EXPECT(ga10b_fw_netlist_find(synth, sz, GA10B_NETC_MAJORV,
                                 &off, &rsz) == -1);
    synth[1] = 4097;                    /* too many regions */
    EXPECT(ga10b_fw_netlist_find(synth, sz, GA10B_NETC_MAJORV,
                                 &off, &rsz) == -1);
    synth_size();
    EXPECT(ga10b_fw_netlist_find(synth, 40, GA10B_NETC_MAJORV,
                                 &off, &rsz) == -1);  /* truncated table */
    synth[4] = 40;                      /* payload inside table */
    EXPECT(ga10b_fw_netlist_find(synth, sz, GA10B_NETC_MAJORV,
                                 &off, &rsz) == -1);
    synth_size();
    synth[4] = 88;                      /* off+size past image end */
    EXPECT(ga10b_fw_netlist_find(synth, sz, GA10B_NETC_MAJORV,
                                 &off, &rsz) == -1);
    synth_size();
    synth[3] = 0;                       /* zero-size region */
    EXPECT(ga10b_fw_netlist_find(synth, sz, GA10B_NETC_MAJORV,
                                 &off, &rsz) == -1);
    synth_size();
    EXPECT(ga10b_fw_netlist_find(synth, 4, GA10B_NETC_MAJORV,
                                 &off, &rsz) == -1);  /* < header */
    EXPECT(ga10b_fw_netlist_find(NULL, sz, GA10B_NETC_MAJORV,
                                 &off, &rsz) == -1);

    /* word-alignment rejections live in the accessor layer */
    synth_size();
    synth[4] = 45;                      /* misaligned offset */
    synth[3] = 3;
    EXPECT(ga10b_netlist_u32(synth, sz, GA10B_NETC_MAJORV, &val) == -1);
    synth_size();
    synth[3] = 6;                       /* misaligned size */
    EXPECT(ga10b_netlist_u32(synth, sz, GA10B_NETC_MAJORV, &val) == -1);
}

static void test_real(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        printf("SKIP real NETC blob (%s not found)\n", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    void* buf = malloc((size_t)len);
    if (buf == NULL || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        printf("FAIL: cannot read %s\n", path);
        fails++;
        fclose(f);
        free(buf);
        return;
    }
    fclose(f);
    uint32_t sz = (uint32_t)len;
    printf("real NETC: %u bytes\n", sz);

    uint32_t val;
    EXPECT(ga10b_netlist_u32(buf, sz, GA10B_NETC_MAJORV, &val) == 0);
    printf("  MAJORV = %08x\n", val);
    EXPECT(ga10b_netlist_u32(buf, sz, GA10B_NETC_BUFFER_SIZE, &val) == 0);
    printf("  BUFFER_SIZE = %u (0x%x)\n", val, val);
    EXPECT(ga10b_netlist_u32(buf, sz, GA10B_NETC_REG_BASE_INDEX, &val) == 0);
    printf("  REG_BASE_INDEX = %u\n", val);

    /* ctxsw ucode bodies must be present + word aligned */
    static const uint32_t bodies[4] = { 0, 1, 2, 3 };
    for (uint32_t k = 0; k < 4; k++) {
        uint32_t off, rsz;
        EXPECT(ga10b_fw_netlist_find(buf, sz, bodies[k], &off, &rsz) == 0);
        EXPECT(rsz > 0 && (rsz & 3u) == 0);
        printf("  region %u: %u bytes @ %u\n", bodies[k], rsz, off);
    }

    /* sw_ctx_load: aiv triples, GR PRI addresses only */
    const uint32_t* w;
    uint32_t n;
    EXPECT(ga10b_netlist_words(buf, sz, GA10B_NETC_SW_CTX_LOAD, &w, &n) == 0);
    EXPECT(n % 3u == 0 && n > 0);
    uint32_t bad = 0;
    for (uint32_t k = 0; k < n / 3u; k++) {
        uint32_t a = w[3u * k];
        if (a < 0x00400000u || a >= 0x00600000u) {
            if (bad < 4)
                printf("  ctx_load addr out of GR range: %08x\n", a);
            bad++;
        }
    }
    EXPECT(bad == 0);
    printf("  SW_CTX_LOAD: %u entries\n", n / 3u);

    /* av lists: pairs */
    static const uint32_t av_ids[3] = {
        GA10B_NETC_SW_BUNDLE_INIT, GA10B_NETC_SW_METHOD_INIT,
        GA10B_NETC_SW_VEID_BUNDLE_INIT
    };
    for (uint32_t k = 0; k < 3; k++) {
        if (ga10b_netlist_words(buf, sz, av_ids[k], &w, &n) == 0) {
            EXPECT(n % 2u == 0);
            printf("  region %u: %u av entries\n", av_ids[k], n / 2u);
        } else {
            printf("  region %u: absent\n", av_ids[k]);
        }
    }
    if (ga10b_netlist_words(buf, sz, GA10B_NETC_SW_BUNDLE64_INIT, &w, &n) == 0) {
        EXPECT(n % 3u == 0);
        printf("  SW_BUNDLE64_INIT: %u entries\n", n / 3u);
    }
    free(buf);
}

int main(int argc, char** argv) {
    test_synthetic();
    test_real(argc > 1 ? argv[1] :
              "/Users/tony/work/gpu-fw/ga10b/NETC_img_prod_encrypted.bin");
    if (fails == 0)
        printf("test_netlist: ALL PASS\n");
    else
        printf("test_netlist: %d FAILURES\n", fails);
    return fails != 0;
}