/* test_nir_sass.c - end-to-end NIR -> sm_87 SASS: build a compute kernel with
 * nir_builder, compile it through nv_be_compile_compute, write the SASS words
 * to probe_nir.bin for the nvdisasm round-trip.  Validates the whole compiler
 * pipeline (frontend NIR -> lowering -> RA -> encoder -> assembly).
 */
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "nv_backend.h"
#include <stdio.h>
#include <stdlib.h>

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

    /* x = tid.x; f = (float)x + 1.0; store_shared(f2i(f), offset=0) */
    nir_def* tid = nir_load_local_invocation_id(&b);
    nir_def* x = nir_channel(&b, tid, 0);
    nir_def* xf = nir_i2f32(&b, x);
    nir_def* sum = nir_fadd(&b, xf, nir_imm_float(&b, 1.0f));
    nir_def* si = nir_f2i32(&b, sum);
    nir_store_shared(&b, si, nir_imm_int(&b, 0), .base = 0);

    struct nv_be_result res;
    char log[1024] = {0};
    if (!nv_be_compile_compute(s, &res, log, sizeof(log))) {
        printf("COMPILE FAIL: %s\n", log);
        return 1;
    }
    printf("NIR->SASS OK: %u instrs, %u GPRs, local_size %ux%ux%u\n",
           res.bin_words / 4, res.meta.num_gprs,
           res.meta.local_size[0], res.meta.local_size[1], res.meta.local_size[2]);

    FILE* f = fopen("probe_nir.bin", "wb");
    fwrite(res.bin, 4, res.bin_words, f);
    fclose(f);
    nv_be_result_free(&res);
    ralloc_free(s);
    return 0;
}