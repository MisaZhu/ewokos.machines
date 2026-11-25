/* Host test: synthesize a cubin from SASS+meta, parse it back with the real
 * projects/kuda cubin parser, and check every reflected field. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv_sass.h"
#include "nv_cubin.h"
#include "cubin.h"

#define CK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); \
} } while (0)

int main(void) {
    /* a tiny kernel: R2 = c[0][0x160]; EXIT */
    nv_asm a; nv_asm_init(&a);
    uint32_t w[4];
    nv_ctrl ct = nv_ctrl_none(); ct.delay = 4;
    nv_mov(w, 2, nv_cb(0, 0x160), ct); nv_asm_emit(&a, w);
    nv_exit(w, ct); nv_asm_emit(&a, w);
    nv_asm_finish(&a);

    struct nv_kernel_meta meta; memset(&meta, 0, sizeof(meta));
    meta.num_gprs = 3;
    meta.param_base = 0x160;
    meta.param_bytes = 8;
    meta.local_size[0] = 64;

    struct nv_cubin_param params[2] = {
        { 0, 0, 4 }, { 1, 4, 4 },
    };
    uint8_t cbank0[0x168]; memset(cbank0, 0, sizeof(cbank0));

    uint32_t need = nv_cubin_build(a.w, a.n * 4, &meta, "ktest",
                                   params, 2, cbank0, sizeof(cbank0), NULL, 0);
    CK(need > 0);
    uint8_t* buf = malloc(need);
    uint32_t got = nv_cubin_build(a.w, a.n * 4, &meta, "ktest",
                                  params, 2, cbank0, sizeof(cbank0), buf, need);
    CK(got == need);

    struct cubin_image img;
    int rc = cubin_parse(&img, buf, got);
    CK(rc == KUBIN_OK);
    CK(img.sm == 87);
    CK(img.nkernels == 1);
    CK(cubin_needs_relocation(&img) == 0);
    CK(cubin_image_size(buf, got) == got);

    const struct cubin_kernel* k = cubin_find_kernel(&img, "ktest");
    CK(k != NULL);
    CK(k->reg_count == 3);
    CK(k->param_base == 0x160);
    CK(k->param_total == 8);
    CK(k->nparams == 2);
    CK(k->params[0].offset == 0 && k->params[0].size == 4);
    CK(k->params[1].offset == 4 && k->params[1].size == 4);
    CK(k->text_size == a.n * 16);
    CK(k->nexit >= 1);
    CK(k->cbank[0].size == sizeof(cbank0));
    CK(memcmp(k->text, a.w, k->text_size) == 0);

    /* param packing round-trip */
    uint32_t p0 = 0x11223344, p1 = 0x55667788;
    void* args[2] = { &p0, &p1 };
    uint8_t packed[8];
    CK(cubin_pack_params(k, args, packed, sizeof(packed)) == KUBIN_OK);
    CK(memcmp(packed, &p0, 4) == 0 && memcmp(packed + 4, &p1, 4) == 0);

    FILE* f = fopen("synth.cubin", "wb");
    fwrite(buf, 1, got, f); fclose(f);
    printf("cubin synth OK: %u bytes, sm%d, %u GPRs, %u params, %u exits\n",
           got, img.sm, k->reg_count, k->nparams, k->nexit);
    free(buf);
    return 0;
}