/* test_nir_build.c - validate the host NIR harness: construct a compute shader
 * with nir_builder (vecadd-style SSBO load/add/store), validate it, and walk
 * the instructions.  Confirms libnirhost.a links and runs before the NIR->SASS
 * backend is exercised end-to-end.
 * Build: see Makefile (links obj/libnirhost.a).
 */
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include <stdio.h>
#include <stdlib.h>

static const nir_shader_compiler_options opts = {
    .lower_fpow = true,
    .lower_fdiv = true,
    .lower_fmod = true,
    .lower_bitops = true,
    .has_fused_comp_and_csel = true,
};

int main(void) {
    nir_shader* s = nir_shader_create(NULL, MESA_SHADER_COMPUTE, &opts);
    s->info.workgroup_size[0] = 8;
    s->info.workgroup_size[1] = 8;
    s->info.workgroup_size[2] = 1;

    nir_function* func = nir_function_create(s, "main");
    nir_function_impl* impl = nir_function_impl_create(func);
    nir_builder b = nir_builder_create(impl);
    b.cursor = nir_before_block(nir_start_block(impl));

    /* tid.x = gl_LocalInvocationID.x */
    nir_def* tid = nir_load_local_invocation_id(&b);
    nir_def* x = nir_channel(&b, tid, 0);
    /* val = x + 1.0 */
    nir_def* one = nir_imm_float(&b, 1.0f);
    nir_def* xf = nir_i2f32(&b, x);
    nir_def* sum = nir_fadd(&b, xf, one);
    nir_def* back = nir_f2i32(&b, sum);
    /* store to a shared var offset (placeholder SSBO) */
    nir_store_shared(&b, back, nir_imm_int(&b, 0), .base = 0);

    nir_validate_shader(s, "test build");

    unsigned nalu = 0, nintr = 0;
    nir_foreach_block(block, impl) {
        nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_alu) nalu++;
            else if (instr->type == nir_instr_type_intrinsic) nintr++;
        }
    }
    printf("NIR harness OK: compute shader built, %u ALU + %u intrinsic instrs\n",
           nalu, nintr);
    ralloc_free(s);
    return 0;
}