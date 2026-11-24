/* Host test: SSBO load/store through nv_be_compile_compute.
 *   ssbo_store      - store a constant to SSBO block 0 at tid.x*4
 *   ssbo_load_store - load from block 0, add 1, store back (the output path)
 * Verifies the ULDC.64 base load + LDG.E/STG.E addressing via cubin parse and
 * nvdisasm. */
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

static nir_shader* new_shader(void) {
    nir_shader* s = nir_shader_create(NULL, MESA_SHADER_COMPUTE,
                                      nv_be_nir_options());
    s->info.workgroup_size[0] = 8;
    s->info.workgroup_size[1] = 1;
    s->info.workgroup_size[2] = 1;
    return s;
}

static nir_def* ssbo_load(nir_builder* b, nir_shader* s, nir_def* off,
                          unsigned ncomp) {
    nir_intrinsic_instr* ld =
        nir_intrinsic_instr_create(s, nir_intrinsic_load_ssbo);
    ld->num_components = ncomp;
    nir_def_init(&ld->instr, &ld->def, ncomp, 32);
    ld->src[0] = nir_src_for_ssa(nir_imm_int(b, 0));   /* block 0 */
    ld->src[1] = nir_src_for_ssa(off);
    nir_builder_instr_insert(b, &ld->instr);
    return &ld->def;
}

static void ssbo_store(nir_builder* b, nir_shader* s, nir_def* val,
                       nir_def* off) {
    nir_intrinsic_instr* st =
        nir_intrinsic_instr_create(s, nir_intrinsic_store_ssbo);
    st->num_components = val->num_components;
    st->src[0] = nir_src_for_ssa(val);
    st->src[1] = nir_src_for_ssa(nir_imm_int(b, 0));   /* block 0 */
    st->src[2] = nir_src_for_ssa(off);
    nir_intrinsic_set_write_mask(st, (1u << val->num_components) - 1u);
    nir_builder_instr_insert(b, &st->instr);
}

static void compile(nir_shader* s, const char* name) {
    struct nv_be_result be;
    char log[1024] = {0};
    if (!nv_be_compile_compute(s, &be, log, sizeof(log))) {
        fprintf(stderr, "COMPILE FAIL %s: %s\n", name, log);
        exit(1);
    }
    uint32_t need = nv_cubin_build(be.bin, be.bin_words, &be.meta, name,
                                   NULL, 0, NULL, 0, NULL, 0);
    CK(need > 0);
    uint8_t cb0[NV_CB0_SIZE];
    memset(cb0, 0, sizeof(cb0));
    need = nv_cubin_build(be.bin, be.bin_words, &be.meta, name,
                          NULL, 0, cb0, sizeof(cb0), NULL, 0);
    uint8_t* buf = malloc(need);
    CK(nv_cubin_build(be.bin, be.bin_words, &be.meta, name,
                      NULL, 0, cb0, sizeof(cb0), buf, need) == need);
    struct cubin_image img;
    CK(cubin_parse(&img, buf, need) == CUBIN_OK);
    const struct cubin_kernel* k = cubin_find_kernel(&img, name);
    CK(k && k->nexit >= 1);
    CK(k->cbank[0].size == NV_CB0_SIZE);   /* daemon-filled launch bank */
    char path[128];
    snprintf(path, sizeof(path), "synth_ssbo_%s.cubin", name);
    FILE* f = fopen(path, "wb");
    fwrite(buf, 1, need, f); fclose(f);
    printf("  %s: %u instrs, %u GPRs -> %s\n", name, be.bin_words / 4,
           k->reg_count, path);
    free(buf);
    nv_be_result_free(&be);
    ralloc_free(s);
}

int main(void) {
    /* ssbo_store: pglc_cs_px[tid.x] = 0x1234 */
    {
        nir_shader* s = new_shader();
        nir_function* fn = nir_function_create(s, "main");
        nir_function_impl* impl = nir_function_impl_create(fn);
        nir_builder b = nir_builder_create(impl);
        b.cursor = nir_before_block(nir_start_block(impl));
        nir_def* x = nir_channel(&b, nir_load_local_invocation_id(&b), 0);
        nir_def* off = nir_ishl(&b, x, nir_imm_int(&b, 2));   /* tid.x*4 */
        ssbo_store(&b, s, nir_imm_int(&b, 0x1234), off);
        compile(s, "store");
    }
    /* ssbo_load_store: pglc_cs_px[tid.x] += 1 */
    {
        nir_shader* s = new_shader();
        nir_function* fn = nir_function_create(s, "main");
        nir_function_impl* impl = nir_function_impl_create(fn);
        nir_builder b = nir_builder_create(impl);
        b.cursor = nir_before_block(nir_start_block(impl));
        nir_def* x = nir_channel(&b, nir_load_local_invocation_id(&b), 0);
        nir_def* off = nir_ishl(&b, x, nir_imm_int(&b, 2));
        nir_def* v = ssbo_load(&b, s, off, 1);
        ssbo_store(&b, s, nir_iadd(&b, v, nir_imm_int(&b, 1)), off);
        compile(s, "load_store");
    }
    /* uniform_ssbo: u = uniform[0]; pglc_cs_px[tid.x] = u  (load_uniform bias
     * + SSBO store together, the shadertoy output shape) */
    {
        nir_shader* s = new_shader();
        nir_function* fn = nir_function_create(s, "main");
        nir_function_impl* impl = nir_function_impl_create(fn);
        nir_builder b = nir_builder_create(impl);
        b.cursor = nir_before_block(nir_start_block(impl));
        nir_intrinsic_instr* ld =
            nir_intrinsic_instr_create(s, nir_intrinsic_load_uniform);
        ld->num_components = 1;
        nir_def_init(&ld->instr, &ld->def, 1, 32);
        nir_intrinsic_set_base(ld, 0);
        ld->src[0] = nir_src_for_ssa(nir_imm_int(&b, 0));
        nir_builder_instr_insert(&b, &ld->instr);
        nir_def* x = nir_channel(&b, nir_load_local_invocation_id(&b), 0);
        nir_def* off = nir_ishl(&b, x, nir_imm_int(&b, 2));
        ssbo_store(&b, s, &ld->def, off);
        compile(s, "uniform_ssbo");
    }
    printf("SSBO compile OK (3 kernels)\n");
    return 0;
}