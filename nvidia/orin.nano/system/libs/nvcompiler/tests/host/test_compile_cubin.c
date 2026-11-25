/* Host test: the compiler-output -> cubin bridge exactly as nv_pglc.cpp's
 * compile_compute_source() does it.  Build a compute NIR, compile to sm_87
 * SASS with nv_be_compile_compute, wrap with nv_cubin_build (no launch-param
 * window; uniforms via cbank0), parse back with the real projects/kuda cubin
 * parser, and emit a cubin for the nvdisasm round-trip. */
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "nv_backend.h"
#include "nv_cubin.h"
#include "cubin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); \
} } while (0)

int main(void) {
    nir_shader* s = nir_shader_create(NULL, MESA_SHADER_COMPUTE,
                                      nv_be_nir_options());
    s->info.workgroup_size[0] = 8;
    s->info.workgroup_size[1] = 8;
    s->info.workgroup_size[2] = 1;
    nir_function* func = nir_function_create(s, "main");
    nir_function_impl* impl = nir_function_impl_create(func);
    nir_builder b = nir_builder_create(impl);
    b.cursor = nir_before_block(nir_start_block(impl));
    nir_def* tid = nir_load_local_invocation_id(&b);
    nir_def* x = nir_channel(&b, tid, 0);
    nir_def* xf = nir_i2f32(&b, x);
    nir_def* sum = nir_fadd(&b, xf, nir_imm_float(&b, 1.0f));
    nir_def* si = nir_f2i32(&b, sum);
    nir_store_shared(&b, si, nir_imm_int(&b, 0), .base = 0);

    struct nv_be_result be;
    char log[1024] = {0};
    CK(nv_be_compile_compute(s, &be, log, sizeof(log)));

    /* same call nv_pglc.cpp makes: name "pgl_cs", no params, no cbank0 image */
    uint32_t need = nv_cubin_build(be.bin, be.bin_words, &be.meta, "pgl_cs",
                                   NULL, 0, NULL, 0, NULL, 0);
    CK(need > 0);
    uint8_t* buf = malloc(need);
    uint32_t got = nv_cubin_build(be.bin, be.bin_words, &be.meta, "pgl_cs",
                                  NULL, 0, NULL, 0, buf, need);
    CK(got == need);

    struct cubin_image img;
    CK(cubin_parse(&img, buf, got) == KUBIN_OK);
    CK(img.sm == 87);
    CK(cubin_needs_relocation(&img) == 0);
    const struct cubin_kernel* k = cubin_find_kernel(&img, "pgl_cs");
    CK(k != NULL);
    CK(k->reg_count == be.meta.num_gprs);
    CK(k->text_size == be.bin_words * 4);
    CK(memcmp(k->text, be.bin, k->text_size) == 0);
    CK(k->nexit >= 1);

    FILE* f = fopen("synth_compile.cubin", "wb");
    fwrite(buf, 1, got, f); fclose(f);
    printf("compile->cubin OK: %u bytes, %u instrs, %u GPRs, %u exits\n",
           got, be.bin_words / 4, k->reg_count, k->nexit);
    free(buf);
    nv_be_result_free(&be);
    ralloc_free(s);
    return 0;
}