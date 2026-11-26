/* Host test: structured control flow through nv_be_compile_compute.
 * Three kernels exercise the branch/phi/return/loop machinery:
 *   if_else_phi  - if/else producing a value (phi resolved by pred-end moves)
 *   early_return - the shadertoy-wrapper bounds check (divergent return)
 *   loop_break   - a loop with a break (back edge + BSSY/BSYNC), no SSA phi
 * Each is compiled, wrapped into a cubin, parsed back, and written for the
 * nvdisasm round-trip. */
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
    s->info.workgroup_size[1] = 8;
    s->info.workgroup_size[2] = 1;
    return s;
}

static void check_cubin(struct nv_be_result* be, const char* name) {
    uint32_t need = nv_cubin_build(be->bin, be->bin_words, &be->meta, name,
                                   NULL, 0, NULL, 0, NULL, 0);
    CK(need > 0);
    uint8_t* buf = malloc(need);
    CK(nv_cubin_build(be->bin, be->bin_words, &be->meta, name,
                      NULL, 0, NULL, 0, buf, need) == need);
    struct cubin_image img;
    CK(cubin_parse(&img, buf, need) == CUBIN_OK);
    const struct cubin_kernel* k = cubin_find_kernel(&img, name);
    CK(k && k->text_size == be->bin_words * 4 && k->nexit >= 1);
    char path[128];
    snprintf(path, sizeof(path), "synth_cf_%s.cubin", name);
    FILE* f = fopen(path, "wb");
    fwrite(buf, 1, need, f); fclose(f);
    printf("  %s: %u instrs, %u GPRs, %u exits -> %s\n",
           name, be->bin_words / 4, k->reg_count, k->nexit, path);
    free(buf);
}

static void compile(nir_shader* s, const char* name) {
    struct nv_be_result be;
    char log[1024] = {0};
    if (!nv_be_compile_compute(s, &be, 1, log, sizeof(log))) {
        fprintf(stderr, "COMPILE FAIL %s: %s\n", name, log);
        exit(1);
    }
    check_cubin(&be, name);
    nv_be_result_free(&be);
    ralloc_free(s);
}

int main(void) {
    /* if_else_phi: r = (tid.x < 4) ? 10 : 20; store_shared(r) */
    {
        nir_shader* s = new_shader();
        nir_function* fn = nir_function_create(s, "main");
        nir_function_impl* impl = nir_function_impl_create(fn);
        nir_builder b = nir_builder_create(impl);
        b.cursor = nir_before_block(nir_start_block(impl));
        nir_def* x = nir_channel(&b, nir_load_local_invocation_id(&b), 0);
        nir_if* nif = nir_push_if(&b, nir_ilt(&b, x, nir_imm_int(&b, 4)));
        nir_def* a = nir_imm_int(&b, 10);
        nir_push_else(&b, nif);
        nir_def* e = nir_imm_int(&b, 20);
        nir_pop_if(&b, nif);
        nir_def* r = nir_if_phi(&b, a, e);
        nir_store_shared(&b, r, nir_imm_int(&b, 0), .base = 0);
        compile(s, "if_else_phi");
    }
    /* early_return: if (tid.x >= 4) return; store_shared(f2i(i2f(x)+1)) */
    {
        nir_shader* s = new_shader();
        nir_function* fn = nir_function_create(s, "main");
        nir_function_impl* impl = nir_function_impl_create(fn);
        nir_builder b = nir_builder_create(impl);
        b.cursor = nir_before_block(nir_start_block(impl));
        nir_def* x = nir_channel(&b, nir_load_local_invocation_id(&b), 0);
        nir_if* nif = nir_push_if(&b, nir_ige(&b, x, nir_imm_int(&b, 4)));
        nir_jump(&b, nir_jump_return);
        nir_pop_if(&b, nif);
        nir_def* f = nir_fadd(&b, nir_i2f32(&b, x), nir_imm_float(&b, 1.0f));
        nir_store_shared(&b, nir_f2i32(&b, f), nir_imm_int(&b, 0), .base = 0);
        compile(s, "early_return");
    }
    /* loop_break: loop { x = load_shared(0); if (x >= 4) break;
     *               store_shared(x + 1, 0) }  (counter in shared, no SSA phi) */
    {
        nir_shader* s = new_shader();
        nir_function* fn = nir_function_create(s, "main");
        nir_function_impl* impl = nir_function_impl_create(fn);
        nir_builder b = nir_builder_create(impl);
        b.cursor = nir_before_block(nir_start_block(impl));
        nir_loop* loop = nir_push_loop(&b);
        nir_def* x = nir_load_shared(&b, 1, 32, nir_imm_int(&b, 0), .base = 0);
        nir_if* nif = nir_push_if(&b, nir_ige(&b, x, nir_imm_int(&b, 4)));
        nir_jump(&b, nir_jump_break);
        nir_pop_if(&b, nif);
        nir_store_shared(&b, nir_iadd(&b, x, nir_imm_int(&b, 1)),
                         nir_imm_int(&b, 0), .base = 0);
        nir_pop_loop(&b, loop);
        compile(s, "loop_break");
    }
    printf("control-flow compile OK (3 kernels)\n");
    return 0;
}